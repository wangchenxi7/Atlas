#ifndef _LINUX_EXTENDED_SYSCALLS_H
#define _LINUX_EXTENDED_SYSCALLS_H

asmlinkage long sys_reset_swap_stats(void);
asmlinkage long sys_get_swap_stats(int __user *on_demand_swapin_num,
				   int __user *prefetch_swapin_num,
				   int __user *hiton_swap_cache_num);
asmlinkage long sys_register_dsa(unsigned long ds_start, unsigned long ds_len,
				 int thd_local, int type);
asmlinkage long sys_deregister_dsa(unsigned long ds_start, int thd_local);

asmlinkage long sys_hermit_dbg(int type, void __user *buf,
			       unsigned long buf_len);

#endif // _LINUX_EXTENDED_SYSCALLS_H