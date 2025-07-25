
# Copyright (c) 2021-2024, PostgreSQL Global Development Group

=pod

=head1 NAME

PostgreSQL::Test::BackgroundPsql - class for controlling background psql processes

=head1 SYNOPSIS

  use PostgreSQL::Test::Cluster;

  my $node = PostgreSQL::Test::Cluster->new('mynode');

  # Create a data directory with initdb
  $node->init();

  # Start the PostgreSQL server
  $node->start();

  # Create and start an interactive psql session
  my $isession = $node->interactive_psql('postgres');
  # Apply timeout per query rather than per session
  $isession->set_query_timer_restart();
  # Run a query and get the output as seen by psql
  my $ret = $isession->query("SELECT 1");
  # Run a backslash command and wait until the prompt returns
  $isession->query_until(qr/postgres #/, "\\d foo\n");
  # Close the session and exit psql
  $isession->quit;

  # Create and start a background psql session
  my $bsession = $node->background_psql('postgres');

  # Run a query which is guaranteed to not return in case it fails
  $bsession->query_safe("SELECT 1");
  # Initiate a command which can be expected to terminate at a later stage
  $bsession->query_until(qr/start/, q(
    \echo start
	CREATE INDEX CONCURRENTLY idx ON t(a);
  ));
  # Close the session and exit psql
  $bsession->quit;

=head1 DESCRIPTION

PostgreSQL::Test::BackgroundPsql contains functionality for controlling
a background or interactive psql session operating on a PostgreSQL node
initiated by PostgreSQL::Test::Cluster.

=cut

package PostgreSQL::Test::BackgroundPsql;

use strict;
use warnings FATAL => 'all';

use Carp;
use Config;
use IPC::Run;
use PostgreSQL::Test::Utils qw(pump_until);
use Test::More;

=pod

=head1 METHODS

=over

=item PostgreSQL::Test::BackgroundPsql->new(interactive, @psql_params, timeout, wait)

Builds a new object of class C<PostgreSQL::Test::BackgroundPsql> for either
an interactive or background session and starts it. If C<interactive> is
true then a PTY will be attached. C<psql_params> should contain the full
command to run psql with all desired parameters and a complete connection
string. For C<interactive> sessions, IO::Pty is required.

This routine will not return until psql has started up and is ready to
consume input. Set B<wait> to 0 to return immediately instead.

=cut

sub new
{
	my $class = shift;
	my ($interactive, $psql_params, $timeout, $wait) = @_;
	my $psql = {
		'stdin' => '',
		'stdout' => '',
		'stderr' => '',
		'query_timer_restart' => undef,
		'query_cnt' => 1,
	};
	my $run;

	# This constructor should only be called from PostgreSQL::Test::Cluster
	my ($package, $file, $line) = caller;
	die
	  "Forbidden caller of constructor: package: $package, file: $file:$line"
	  unless $package->isa('PostgreSQL::Test::Cluster') || $package->isa('PostgresNode');

	$psql->{timeout} = IPC::Run::timeout(
		defined($timeout)
		? $timeout
		: $PostgreSQL::Test::Utils::timeout_default);

	if ($interactive)
	{
		$run = IPC::Run::start $psql_params,
		  '<pty<', \$psql->{stdin}, '>pty>', \$psql->{stdout}, '2>',
		  \$psql->{stderr},
		  $psql->{timeout};
	}
	else
	{
		$run = IPC::Run::start $psql_params,
		  '<', \$psql->{stdin}, '>', \$psql->{stdout}, '2>', \$psql->{stderr},
		  $psql->{timeout};
	}

	$psql->{run} = $run;

	my $self = bless $psql, $class;

	$wait = 1 unless defined($wait);
	if ($wait)
	{
		$self->wait_connect();
	}

	return $self;
}

=pod

=item $session->wait_connect

Returns once psql has started up and is ready to consume input. This is called
automatically for clients unless requested otherwise in the constructor.

=cut

sub wait_connect
{
	my ($self) = @_;

	# Request some output, and pump until we see it.  This means that psql
	# connection failures are caught here, relieving callers of the need to
	# handle those.  (Right now, we have no particularly good handling for
	# errors anyway, but that might be added later.)
	#
	# See query() for details about why/how the banner is used.
	my $banner = "background_psql: ready";
	my $banner_match = qr/(^|\n)$banner\r?\n/;
	$self->{stdin} .= "\\echo $banner\n\\warn $banner\n";
	$self->{run}->pump()
	  until ($self->{stdout} =~ /$banner_match/
		  && $self->{stderr} =~ /$banner\r?\n/)
	  || $self->{timeout}->is_expired;

	note "connect output:\n",
	  explain {
		stdout => $self->{stdout},
		stderr => $self->{stderr},
	  };

	# clear out banners
	$self->{stdout} = '';
	$self->{stderr} = '';

	die "psql startup timed out" if $self->{timeout}->is_expired;
}

=pod

=item $session->quit

Close the session and clean up resources. Each test run must be closed with
C<quit>.

=cut

sub quit
{
	my ($self) = @_;

	$self->{stdin} .= "\\q\n";

	return $self->{run}->finish;
}

=pod

=item $session->reconnect_and_clear

Terminate the current session and connect again.

=cut

sub reconnect_and_clear
{
	my ($self) = @_;

	# If psql isn't dead already, tell it to quit as \q, when already dead,
	# causes IPC::Run to unhelpfully error out with "ack Broken pipe:".
	$self->{run}->pump_nb();
	if ($self->{run}->pumpable())
	{
		$self->{stdin} .= "\\q\n";
	}
	$self->{run}->finish;

	# restart
	$self->{run}->run();
	$self->{stdin} = '';
	$self->{stdout} = '';

	$self->wait_connect();
}

=pod

=item $session->query()

Executes a query in the current session and returns the output in scalar
context and (output, error) in list context where error is 1 in case there
was output generated on stderr when executing the query.

=cut

sub query
{
	my ($self, $query) = @_;
	my $ret;
	my $output;
	my $query_cnt = $self->{query_cnt}++;

	local $Test::Builder::Level = $Test::Builder::Level + 1;

	note "issuing query $query_cnt via background psql: $query";

	$self->{timeout}->start() if (defined($self->{query_timer_restart}));

	# Feed the query to psql's stdin, followed by \n (so psql processes the
	# line), by a ; (so that psql issues the query, if it doesn't include a ;
	# itself), and a separator echoed both with \echo and \warn, that we can
	# wait on.
	#
	# To avoid somehow confusing the separator from separately issued queries,
	# and to make it easier to debug, we include a per-psql query counter in
	# the separator.
	#
	# We need both \echo (printing to stdout) and \warn (printing to stderr),
	# because on windows we can get data on stdout before seeing data on
	# stderr (or vice versa), even if psql printed them in the opposite
	# order. We therefore wait on both.
	#
	# We need to match for the newline, because we try to remove it below, and
	# it's possible to consume just the input *without* the newline. In
	# interactive psql we emit \r\n, so we need to allow for that. Also need
	# to be careful that we don't e.g. match the echoed \echo command, rather
	# than its output.
	my $banner = "background_psql: QUERY_SEPARATOR $query_cnt:";
	my $banner_match = qr/(^|\n)$banner\r?\n/;
	$self->{stdin} .= "$query\n;\n\\echo $banner\n\\warn $banner\n";
	pump_until(
		$self->{run}, $self->{timeout},
		\$self->{stdout}, qr/$banner_match/);
	pump_until(
		$self->{run}, $self->{timeout},
		\$self->{stderr}, qr/$banner_match/);

	die "psql query timed out" if $self->{timeout}->is_expired;

	note "results query $query_cnt:\n",
	  explain {
		stdout => $self->{stdout},
		stderr => $self->{stderr},
	  };

	# Remove banner from stdout and stderr, our caller doesn't care.  The
	# first newline is optional, as there would not be one if consuming an
	# empty query result.
	$output = $self->{stdout};
	$output =~ s/$banner_match//;
	$self->{stderr} =~ s/$banner_match//;

	# clear out output for the next query
	$self->{stdout} = '';

	$ret = $self->{stderr} eq "" ? 0 : 1;

	return wantarray ? ($output, $ret) : $output;
}

=pod

=item $session->query_safe()

Wrapper around C<query> which errors out if the query failed to execute.
Query failure is determined by it producing output on stderr.

=cut

sub query_safe
{
	my ($self, $query) = @_;

	my $ret = $self->query($query);

	if ($self->{stderr} ne "")
	{
		die "query failed: $self->{stderr}";
	}

	return $ret;
}

=pod

=item $session->query_until(until, query)

Issue C<query> and wait for C<until> appearing in the query output rather than
waiting for query completion. C<query> needs to end with newline and semicolon
(if applicable, interactive psql input may not require it) for psql to process
the input.

=cut

sub query_until
{
	my ($self, $until, $query) = @_;
	my $ret;
	local $Test::Builder::Level = $Test::Builder::Level + 1;

	$self->{timeout}->start() if (defined($self->{query_timer_restart}));
	$self->{stdin} .= $query;

	pump_until($self->{run}, $self->{timeout}, \$self->{stdout}, $until);

	die "psql query timed out" if $self->{timeout}->is_expired;

	$ret = $self->{stdout};

	# clear out output for the next query
	$self->{stdout} = '';

	return $ret;
}

=pod

=item $session->set_query_timer_restart()

Configures the timer to be restarted before each query such that the defined
timeout is valid per query rather than per test run.

=back

=cut

sub set_query_timer_restart
{
	my $self = shift;

	$self->{query_timer_restart} = 1;
	return $self->{query_timer_restart};
}

1;
