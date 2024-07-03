/**
 * adc_macros.h - global macros for ADC|RMGrid|Canvas|Hermit
 */

#ifndef _LINUX_ADC_MACROS_H
#define _LINUX_ADC_MACROS_H

#define RSWAP_KERNEL_SUPPORT 3
#define RMGRID_CPU_FREQ 2795 // in MHz
#define RMGRID_NR_PCORE 24 // #(physical cores)
#define RMGRID_NR_HCORE (2 * RMGRID_NR_PCORE) // #(hyper cores)
// # maximum memory supported in KB. For now support 400GB at most.
#define RMGRID_MAX_MEM (400UL * 1024 * 1024 * 1024)

#define ADC_PROFILE_PF_BREAKDOWN
#define HERMIT_DBG_PF_TRACE
// #define ADC_PROFILE_IPC 1 // Must define 1 here to make __is_defined() work
// #define ADC_VM


/* utils */
#define adc_safe_div(n, base) ((base) ? ((n) / (base)) : 0)

#endif /* _LINUX_ADC_MACROS_H */
