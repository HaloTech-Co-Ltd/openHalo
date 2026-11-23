#ifndef HALO_PMC_H
#define HALO_PMC_H

typedef struct HaloPmc HaloPmc;

extern char *halo_pmc_list;
extern bool halo_pmc_segment;

extern HaloPmc *HaloPmcBegin(void);
extern bool HaloPmcSegmentEnabled(const HaloPmc *pmc);
extern void HaloPmcBeforeTuple(HaloPmc *pmc);
extern void HaloPmcAfterTuple(HaloPmc *pmc);
extern void HaloPmcReport(HaloPmc *pmc);
extern void HaloPmcClose(HaloPmc *pmc);

#endif
