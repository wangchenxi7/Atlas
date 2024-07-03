#include <linux/swap_stats.h>
#include <linux/syscalls.h>
#include <linux/printk.h>
#include <linux/hermit.h>

SYSCALL_DEFINE0(reset_swap_stats)
{
	reset_adc_swap_stats();
	reset_adc_pf_breakdown();
	return 0;
}

SYSCALL_DEFINE3(get_swap_stats, int __user *, ondemand_swapin_num, int __user *,
		prefetch_swapin_num, int __user *, hit_on_prefetch_num)
{
	int dmd_swapin_num;
	int prf_swapin_num;
	int hit_prftch_num;
	int swapout_num;

	report_adc_time_stat();
	report_adc_counters();
	report_adc_pf_breakdown(NULL);

	dmd_swapin_num = get_adc_counter(ADC_ONDEMAND_SWAPIN);
	prf_swapin_num = get_adc_counter(ADC_PREFETCH_SWAPIN);
	hit_prftch_num = get_adc_counter(ADC_HIT_ON_PREFETCH);
	swapout_num = get_adc_counter(ADC_SWAPOUT);

	put_user(dmd_swapin_num, ondemand_swapin_num);
	put_user(prf_swapin_num, prefetch_swapin_num);
	put_user(hit_prftch_num, hit_on_prefetch_num);

	return 0;
}

SYSCALL_DEFINE4(register_dsa, unsigned long, dsa_start, unsigned long, dsa_len,
		int, thd_local, int, type)
{
	insert_dsa(dsa_start, dsa_len, thd_local, type);
	return 0;
}

SYSCALL_DEFINE2(deregister_dsa, unsigned long, dsa_start, int, thd_local)
{
	remove_dsa(dsa_start, thd_local);
	return 0;
}

SYSCALL_DEFINE3(hermit_dbg, int, type, void __user *, usr_buf, unsigned long,
		buf_len)
{
	void *buf = kmalloc(buf_len, GFP_KERNEL);
	if (copy_from_user(buf, usr_buf, buf_len))
		return -EINVAL;
	hermit_dbg(type, buf, buf_len);
	return 0;
}