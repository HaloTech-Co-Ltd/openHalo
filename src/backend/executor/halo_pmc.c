/* Linux CPU performance counters for private hash table builds. */
#include "postgres.h"

#include "catalog/namespace.h"
#include "executor/halo_pmc.h"
#include "executor/spi.h"
#include "utils/lsyscache.h"
#include "utils/varlena.h"

#ifdef __linux__
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

#define HALO_PMC_MAX_EVENTS 8
#define HALO_PMC_NAME_SIZE 41

char *halo_pmc_list = NULL;
bool halo_pmc_segment = false;

typedef struct HaloPmcEvent
{
	char		name[HALO_PMC_NAME_SIZE];
	uint32		type;
	uint64		config;
	uint64		config1;
	uint64		config2;
	int			fd;
	uint64		id;
#ifdef __linux__
	struct perf_event_mmap_page *page;
#endif
	uint64		before;
	uint64		before_unscheduled;
	uint64		total;
} HaloPmcEvent;

struct HaloPmc
{
	HaloPmcEvent events[HALO_PMC_MAX_EVENTS];
	int			count;
	Size		page_size;
	bool		segment;
	bool		before_valid;
	uint64		measured;
	uint64		skipped;
};

static bool
PmcInit(HaloPmc *pmc)
{
	char	   *names = pstrdup(halo_pmc_list);
	List	   *requested = NIL;
	ListCell   *cell;
	Oid			schema;
	bool		valid = true;
	int			result;

	if (!SplitIdentifierString(names, ',', &requested) ||
		list_length(requested) == 0 || list_length(requested) > HALO_PMC_MAX_EVENTS)
	{
		ereport(NOTICE, (errmsg("halo_pmc: configure 1 to %d comma-separated event names",
								HALO_PMC_MAX_EVENTS)));
		valid = false;
		goto cleanup;
	}

	schema = get_namespace_oid("public", true);
	if (!OidIsValid(schema) || !OidIsValid(get_relname_relid("pmc_name", schema)))
	{
		ereport(NOTICE, (errmsg("halo_pmc: initialize public.pmc_name before measuring")));
		valid = false;
		goto cleanup;
	}

	if (SPI_connect() != SPI_OK_CONNECT)
		elog(ERROR, "halo_pmc: SPI_connect failed");

	/*
	 * Seed metadata from SQL before opening any descriptors. Reload it for
	 * every measured build so changes to the table take effect on the next
	 * build. Convert to fixed types before reading the Datums below.
	 */
	result = SPI_execute("SELECT pg_catalog.lower(pcm_name::text), "
						 "pmutype::pg_catalog.int4, pe_config::pg_catalog.int8, "
						 "config1::pg_catalog.int8, config2::pg_catalog.int8 "
						 "FROM public.pmc_name", true, 0);
	if (result != SPI_OK_SELECT)
		elog(ERROR, "halo_pmc: failed to read public.pmc_name");

	foreach(cell, requested)
	{
		const char *name = lfirst(cell);
		HaloPmcEvent *event = &pmc->events[pmc->count];
		uint64		i;
		int			j;
		bool		found = false;

		for (j = 0; j < pmc->count; ++j)
			if (pg_strcasecmp(name, pmc->events[j].name) == 0)
				break;
		if (j < pmc->count || strlen(name) >= sizeof(event->name))
		{
			ereport(NOTICE, (errmsg("halo_pmc: invalid or duplicate event name \"%s\"", name)));
			valid = false;
			break;
		}

		for (i = 0; i < SPI_processed; ++i)
		{
			HeapTuple	row = SPI_tuptable->vals[i];
			TupleDesc	desc = SPI_tuptable->tupdesc;
			const char *rowname = SPI_getvalue(row, desc, 1);
			Datum		fields[4];
			bool		isnull;
			bool		hasnull = false;

			if (rowname == NULL || pg_strcasecmp(rowname, name) != 0)
				continue;
			for (j = 0; j < lengthof(fields); ++j)
			{
				fields[j] = SPI_getbinval(row, desc, j + 2, &isnull);
				hasnull |= isnull;
			}
			if (found || hasnull || DatumGetInt32(fields[0]) < 0)
			{
				valid = false;
				break;
			}
			found = true;
			memcpy(event->name, name, strlen(name) + 1);
			event->type = DatumGetInt32(fields[0]);
			event->config = DatumGetInt64(fields[1]);
			event->config1 = DatumGetInt64(fields[2]);
			event->config2 = DatumGetInt64(fields[3]);
			event->fd = -1;
		}
		if (!found || !valid)
		{
			ereport(NOTICE, (errmsg("halo_pmc: missing or invalid event \"%s\"", name)));
			valid = false;
			break;
		}
		pmc->count++;
	}
	SPI_finish();

cleanup:
	list_free(requested);
	pfree(names);
	return valid;
}

HaloPmc *
HaloPmcBegin(void)
{
	HaloPmc    *pmc;
#ifdef __linux__
	int			leader = -1;
	int			saved_errno;
	const char *stage;
	const char *event_name = "";
	int			i;
#endif

	if (halo_pmc_list == NULL || halo_pmc_list[0] == '\0')
		return NULL;
	pmc = palloc0(sizeof(*pmc));
	pmc->segment = halo_pmc_segment;
	if (!PmcInit(pmc))
	{
		pfree(pmc);
		return NULL;
	}

#ifdef __linux__
	for (i = 0; i < pmc->count; ++i)
	{
		HaloPmcEvent *event = &pmc->events[i];
		struct perf_event_attr attr = {0};

		attr.size = sizeof(attr);
		attr.type = event->type;
		attr.config = event->config;
		attr.config1 = event->config1;
		attr.config2 = event->config2;
		attr.disabled = (leader == -1);
		attr.exclude_kernel = 1;
		attr.exclude_hv = 1;
		attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED |
			PERF_FORMAT_TOTAL_TIME_RUNNING | PERF_FORMAT_ID;

		stage = "open";
		event_name = event->name;
		event->fd = syscall(SYS_perf_event_open, &attr, 0, -1, leader,
							PERF_FLAG_FD_CLOEXEC);
		if (event->fd < 0)
			goto failed;
		if (leader == -1)
			leader = event->fd;
		stage = "get event ID for";
		if (ioctl(event->fd, PERF_EVENT_IOC_ID, &event->id) < 0)
			goto failed;
	}

#if defined(__x86_64__) || defined(__i386__)
	if (pmc->segment)
	{
		long		size = sysconf(_SC_PAGESIZE);

		if (size > 0)
		{
			pmc->page_size = size;
			for (i = 0; i < pmc->count; ++i)
			{
				HaloPmcEvent *event = &pmc->events[i];
				void	   *map = mmap(NULL, size, PROT_READ, MAP_SHARED,
									event->fd, 0);

				if (map != MAP_FAILED)
					event->page = map;
			}
		}
	}
#endif

	stage = "enable group starting with";
	event_name = pmc->events[0].name;
	if (ioctl(leader, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) < 0)
		goto failed;
	return pmc;

failed:
	/* Copy before releasing the event array. */
	{
		char		name[HALO_PMC_NAME_SIZE];

		saved_errno = errno;
		memcpy(name, event_name, strlen(event_name) + 1);
		HaloPmcClose(pmc);
		errno = saved_errno;
		ereport(NOTICE, (errmsg("halo_pmc: could not %s %s: %m", stage, name)));
	}
#else
	HaloPmcClose(pmc);
	ereport(NOTICE, (errmsg("halo_pmc: hardware monitoring requires Linux")));
#endif
	return NULL;
}

bool
HaloPmcSegmentEnabled(const HaloPmc *pmc)
{
	return pmc != NULL && pmc->segment;
}

static bool
PmcSnapshot(HaloPmcEvent *event, uint64 *value, uint64 *unscheduled)
{
#if defined(__linux__) && (defined(__x86_64__) || defined(__i386__))
	const struct perf_event_mmap_page *page = event->page;
	int			retry;

	if (page == NULL)
		return false;
	for (retry = 0; retry < 3; ++retry)
	{
		uint32		seq = __atomic_load_n(&page->lock, __ATOMIC_ACQUIRE);
		uint32		index;
		uint16		width;
		uint32		lo;
		uint32		hi;
		uint64		raw;
		uint64		offset;
		uint64		gap;

		if (seq & 1)
			continue;
		index = page->index;
		width = page->pmc_width;
		offset = (uint64) page->offset;
		gap = page->time_enabled - page->time_running;
		if (!page->cap_user_rdpmc || index == 0 || width == 0 || width > 64)
			return false;

		__asm__ volatile("lfence\n\trdpmc\n\tlfence"
						 : "=a"(lo), "=d"(hi) : "c"(index - 1) : "memory");
		raw = ((uint64) hi << 32) | lo;
		if (width < 64)
		{
			uint64		sign = UINT64CONST(1) << (width - 1);

			raw &= (sign << 1) - 1;
			raw = (raw ^ sign) - sign;
		}
		__atomic_thread_fence(__ATOMIC_ACQUIRE);
		if (__atomic_load_n(&page->lock, __ATOMIC_RELAXED) != seq)
			continue;
		*value = offset + raw;
		*unscheduled = gap;
		return true;
	}
#endif
	return false;
}

void
HaloPmcBeforeTuple(HaloPmc *pmc)
{
	int			i;

	pmc->before_valid = false;
	for (i = 0; i < pmc->count; ++i)
	{
		HaloPmcEvent *event = &pmc->events[i];

		if (!PmcSnapshot(event, &event->before, &event->before_unscheduled))
			return;
	}
	pmc->before_valid = true;
}

void
HaloPmcAfterTuple(HaloPmc *pmc)
{
	uint64		deltas[HALO_PMC_MAX_EVENTS];
	int			i;

	if (!pmc->before_valid)
		goto skipped;

	/* Snapshot in reverse order to minimize work inside the innermost pair. */
	for (i = pmc->count - 1; i >= 0; --i)
	{
		HaloPmcEvent *event = &pmc->events[i];
		uint64		value;
		uint64		unscheduled;

		if (!PmcSnapshot(event, &value, &unscheduled) ||
			unscheduled != event->before_unscheduled || value < event->before)
			goto skipped;
		deltas[i] = value - event->before;
	}
	for (i = 0; i < pmc->count; ++i)
		pmc->events[i].total += deltas[i];
	pmc->measured++;
	return;

skipped:
	pmc->skipped++;
}

void
HaloPmcReport(HaloPmc *pmc)
{
#ifdef __linux__
	int			leader = pmc->events[0].fd;
	int			i;
	struct
	{
		uint64		nr;
		uint64		enabled;
		uint64		running;
		struct
		{
			uint64		value;
			uint64		id;
		}			events[HALO_PMC_MAX_EVENTS];
	}			counts;
	ssize_t		nread;

	if (ioctl(leader, PERF_EVENT_IOC_DISABLE, PERF_IOC_FLAG_GROUP) < 0)
	{
		ereport(NOTICE, (errmsg("halo_pmc: could not disable counter group: %m")));
		return;
	}

	if (pmc->segment)
	{
		ereport(NOTICE,
				(errmsg("halo_pmc: private hash tuple body RDPMC measured=" UINT64_FORMAT
						" skipped=" UINT64_FORMAT, pmc->measured, pmc->skipped)));
		if (pmc->measured > 0)
			for (i = 0; i < pmc->count; ++i)
				ereport(NOTICE,
						(errmsg("halo_pmc: %s tuple-body raw delta=" UINT64_FORMAT,
								pmc->events[i].name, pmc->events[i].total)));
	}

	do
		nread = read(leader, &counts, sizeof(counts));
	while (nread < 0 && errno == EINTR);
	if (nread != (ssize_t) (3 * sizeof(uint64) +
							pmc->count * sizeof(counts.events[0])) ||
		counts.nr != pmc->count || counts.enabled == 0 || counts.running == 0)
	{
		ereport(NOTICE, (errmsg("halo_pmc: private hash build group yielded no usable counts")));
		return;
	}

	for (i = 0; i < pmc->count; ++i)
	{
		int			j;

		for (j = 0; j < pmc->count; ++j)
			if (pmc->events[j].id == counts.events[i].id)
			{
				ereport(NOTICE,
						(errmsg("halo_pmc: private hash build %s scaled=%.0f scheduled=%.2f%% raw=" UINT64_FORMAT,
								pmc->events[j].name,
								(double) counts.events[i].value * counts.enabled / counts.running,
								100.0 * counts.running / counts.enabled,
								counts.events[i].value)));
				break;
			}
	}
#endif
}

void
HaloPmcClose(HaloPmc *pmc)
{
#ifdef __linux__
	int			i;

	for (i = pmc->count - 1; i >= 0; --i)
	{
		HaloPmcEvent *event = &pmc->events[i];

		if (event->page != NULL)
			munmap(event->page, pmc->page_size);
		if (event->fd >= 0)
			close(event->fd);
	}
#endif
	pfree(pmc);
}
