# Contributing

### Getting started
openHalo is developed on GitHub. Anyone who wishes to contribute to it must have a Github account and be familiar with Git tools and vscode workflow.
Once you have your GitHub account, fork this repository so that you can have your private copy to start hacking on and to use as a source of pull requests.

#### Server Setup
- Create User & User Group
```sh
groupadd -g 1000 halo
useradd -u 1000 -g halo halo
```

- Setup Environment Variables
```sh
sudo mkdir /var/run/openhalo
sudo chown halo:halo /var/run/openhalo

export HALO_HOME=/home/halo/openhalo/1.0
export PGDATA=/home/halo/ohdata
export PATH=$HALO_HOME/bin:$PATH
export LD_LIBRARY_PATH=$HALO_HOME/lib
export PGHOST=/var/run/openhalo
export COPT='-Werror'
```
#### Install from your source code
- uuid is mandatory.

```sh
# --with-uuid=ossp can be replaced with --with-uuid=e2fs
./configure --prefix=/home/halo/openhalo/1.0 --enable-debug --with-uuid=ossp --with-icu CFLAGS=-O0
make && make install
cd contrib
make && make install
```

- Initialize Database
```sh
pg_ctl init -D $PGDATA
```

- Server Configuration
```
...
database_compat_mode = 'mysql'                 # database compat mode, values can be:
                                               # mysql (mysql mode)
                                               # postgresql (default)

mysql.listener_on = true                       # (enable MySQL listener; change requires restart)
mysql.port = 3306                              # (port for MySQL; change requires restart)
...
```

- Start & Initialize MySQL Environment
```sql
psql -p 5432

# CREATE EXTENSION aux_mysql CASCADE;
```

### Debugging
1. Identify the Process: We may use the MySQL driver to connect to openHalo, and then run `SELECT pg_backend_pid()` to obtain the pid. For example:
```
[halo@halo-centos8 ~]$ mysql -P 3306 -h 127.0.0.1 -uhalo
Welcome to the MySQL monitor.  Commands end with ; or \g.
Your MySQL connection id is 1
Server version: 5.7.32-log-zm MySQL Server (GPL)

Copyright (c) 2000, 2021, Oracle and/or its affiliates.

Oracle is a registered trademark of Oracle Corporation and/or its
affiliates. Other names may be trademarks of their respective
owners.

Type 'help;' or '\h' for help. Type '\c' to clear the current input statement.

mysql> CREATE DATABASE test;
Query OK, 0 rows affected (0.00 sec)

mysql> use test;
Database changed
mysql> SELECT pg_backend_pid();
+----------------+
| pg_backend_pid |
+----------------+
|        1773087 |
+----------------+
1 row in set (0.00 sec)

mysql> 
```
This pid (1773087 in this case) indicates the process that you should attach the debugger to.

2. Start Debugging: Press F5 to start debugging. When prompted, you'll need to attach the debugger to the appropriate Postgres process.

3. Set Breakpoints and Debug: With the debugger attached, you can set breakpoints within the code. This allows you to step through the code execution, inspect variables, and fully debug the Postgres instance running in your container.

## Testing
Currently, the standard regression tests using openHalo are employed. They are targeted at the PostgreSQL mode of openHalo. To run the tests, please execute the `make check` command. For the regression tests of MySQL, we will introduce Python to handle them in the future.