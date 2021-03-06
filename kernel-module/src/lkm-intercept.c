#include <linux/module.h>
#include <linux/mman.h>
#include <asm/uaccess.h>       /* for get_user and put_user */
#include <linux/sched.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/vmalloc.h>
#include <asm/prctl.h>
#include <linux/slab.h>

#include "lkm-config.h"
#include "lkm-utility.h"
#include "lkm-hook.h"
#include "lkm-monitor.h"
#include "lkm-cc-ss.h"
#include "lkm-netlink.h"
#include "lkm-file.h"

char *get_start_encode_and_set_entry(struct task_struct *ts, long program_entry);
/*
	mov %eax, $231//exit_group
	syscall
*/
char check_instr_encode[CHECK_ENCODE_LEN] = {0xb8, 0xe7, 0x00, 0x00, 0x00, 0x0f, 0x05};

//set checkpoint to check the program is begin to execute __start function
void set_checkpoint(struct task_struct *ts, long program_entry, long x_region_start, long x_region_end)
{
	int index;
	char *record_encode = get_start_encode_and_set_entry(ts, program_entry);
	char *target_encode = (char*)program_entry;
	orig_mprotect(x_region_start, x_region_end - x_region_start, PROT_READ|PROT_WRITE|PROT_EXEC);
	//record the original encode
	for(index=0; index<CHECK_ENCODE_LEN; index++)
		get_user(record_encode[index], target_encode+index);
	//rewrite the checkpoint encode
	for(index=0; index<CHECK_ENCODE_LEN; index++)
		put_user(check_instr_encode[index], target_encode+index);
}

ulong is_code_cache(char *name)
{
	if(name && strstr(name, ".cc"))
		return 1;
	else
		return 0;
}

void protect_orig_x_region(struct task_struct *ts, char app_slot_idx)
{
	int shuffle_pid = get_shuffle_pid(app_slot_idx);
	if(shuffle_pid!=0 && strcmp(ts->comm, "dedup")){
		struct mm_struct *mm = ts->mm;
		struct vm_area_struct *list = mm->mmap, *ptr = list;
		do{
			//TODO: need protect [vdso] region
			struct file *fil = ptr->vm_file;
			if(fil != NULL){
				char* name = fil->f_path.dentry->d_iname;
				if(is_code_cache(name)==0 && ptr->vm_page_prot.pgprot==PAGE_COPY_EXECV && ptr->vm_start!=(0x400000+CC_OFFSET)){
					orig_mprotect(ptr->vm_start, ptr->vm_end - ptr->vm_start, PROT_READ);
					PRINTK("protect %lx - %lx %s\n", ptr->vm_start, ptr->vm_end, name);
				}
			}

			ptr = ptr->vm_next;
			if(ptr == NULL) break;
		} while (ptr != list);
		
		PRINTK("Protect origin x region!\n");
	}
}

void send_init_mesg_to_shuffle_process(struct task_struct *ts, char app_slot_idx)
{
	struct pt_regs *regs = task_pt_regs(ts);
	long curr_ip = regs->ip - CHECK_ENCODE_LEN;
	volatile char *start_flag = req_a_start_flag(app_slot_idx, ts->pid);
	int shuffle_pid = get_shuffle_pid(app_slot_idx);
	MESG_BAG msg = {P_PROCESS_IS_IN, ts->pid, curr_ip, {0}, CC_OFFSET, SS_OFFSET, GS_BASE, global_ss_type, \
			"\0", "init code cache and request code variant!"};
	strcpy(msg.app_name, ts->comm);
	
	if(shuffle_pid!=0){
		nl_send_msg(shuffle_pid, msg);
		*start_flag = 1;
		while(*start_flag){
			schedule();
		}
		regs->ip = get_shuffle_pc(app_slot_idx);
		
		free_a_start_flag(app_slot_idx, ts->pid);
	}else
		regs->ip = curr_ip;
}

//set checkpoint to check the program is begin to execute __start function
long set_program_start(struct task_struct *ts, char *orig_encode, char app_slot_idx)
{
	int index;
	int ret;
	char *target_encode = (char*)set_app_start(ts);
	//rewrite the checkpoint encode
	for(index=0; index<CHECK_ENCODE_LEN; index++)
		put_user(orig_encode[index], target_encode+index);
	//set gs base
	if(need_set_gs()){
		ret = orig_arch_prctl(ARCH_SET_GS, GS_BASE);
		if(ret!=0)
			PRINTK("[LKM] set gs base error!\n");
	}
	//send msg to generate the cc and get the pc
	send_init_mesg_to_shuffle_process(ts, app_slot_idx);
	//protect the origin x region
	protect_orig_x_region(current, app_slot_idx);
	
	return _START_RAX;
}

void replace_at_base_in_aux(ulong orig_interp_base, ulong new_interp_base, struct task_struct *ts)
{
	ulong *elf_info = ts->mm->saved_auxv;
	ulong interp_base = 0;
	struct pt_regs *regs = task_pt_regs(ts);
	ulong *auxv_rsp = (long*)regs->sp;
	ulong *auxv_sysinfo = NULL;
	ulong *auxv_sysinfo_ehdr = NULL;
	ulong sysinfo_val = 0;
	ulong sysinfo_ehdr_val = 0;
	//interp_base in saved_auxv
	while(*elf_info!=orig_interp_base){
		elf_info++;
	}
	*elf_info = new_interp_base;
	//interp_base in stack 
	while(interp_base!=orig_interp_base){
		get_user(interp_base, auxv_rsp);
		auxv_rsp++;
		if(interp_base==AT_SYSINFO){//close vdso area
			auxv_sysinfo = auxv_rsp;
			get_user(sysinfo_val, auxv_rsp);
			put_user(0, auxv_rsp);
		}else if(interp_base==AT_SYSINFO_EHDR){
			auxv_sysinfo_ehdr = auxv_rsp;
			get_user(sysinfo_ehdr_val, auxv_rsp);
			put_user(0, auxv_rsp);
		}
	}
	auxv_rsp--;
	put_user(new_interp_base, auxv_rsp);
	PRINTK("SYSINFO: %lx, SYSINFO_EHDR: %lx\n", sysinfo_val, sysinfo_ehdr_val);
}

//when gdb debug the program, the ld.so is loaded at the very high address, there is no space to allocate the memory
//so we remap the interpreter and allocate the code cache for main and ld.so by the way
void remmap_interp_and_allocate_cc(struct task_struct *ts)
{
	typedef struct {
		ulong region_start;
		ulong region_len;
		ulong prot;
		ulong flags;
		ulong off;
		struct file *file_ptr;
	}LD_REGION;
	#define LD_REGION_MAX 4
	LD_REGION ld_regions[LD_REGION_MAX];
	int ld_region_num = 0;
	int index = 0;
	struct mm_struct *mm = ts->mm;
	struct vm_area_struct *list = mm->mmap, *ptr = list;
	struct pt_regs *regs = task_pt_regs(ts);
#ifdef _VM	
	long ld_bss_start = 0;
#endif
	long map_addr = 0;
	long cc_ret = 0;
	long ld_offset = 0;//ld-linux.so fixed offset
	long program_entry = 0;
	long *program_entry_addr = NULL;
	int has_handle_main_elf = 0;
#ifdef _C10	
	char buf[256];
	char *ld_path = NULL;
	int ld_fd = 0;
	void *bk_buf = NULL;
#endif

	do{
		struct file *fil = ptr->vm_file;
		if(fil != NULL){
			char* name = fil->f_path.dentry->d_iname;
			if(name && strcmp(name, LD_NAME)==0){
				ld_regions[ld_region_num].region_start = ptr->vm_start;
				ld_regions[ld_region_num].region_len = ptr->vm_end - ptr->vm_start;					

				if(ld_region_num==0)
					ld_regions[ld_region_num].prot = PROT_READ|PROT_EXEC;
				else if(ld_region_num==1)
					ld_regions[ld_region_num].prot = PROT_READ|PROT_WRITE;
				else 
					ld_regions[ld_region_num].prot = 0;
				
				ld_regions[ld_region_num].flags = MAP_PRIVATE|MAP_DENYWRITE|MAP_FIXED;
				ld_regions[ld_region_num].off = ptr->vm_pgoff<<PAGE_SHIFT;
				ld_regions[ld_region_num].file_ptr = fil;
				ld_region_num++;
#ifdef _VM				
				ld_bss_start = ptr->vm_end;
#endif
			}

			if(name && is_monitor_app(name) && !has_handle_main_elf && ptr->vm_page_prot.pgprot==PAGE_COPY_EXECV){//main file executable region
				cc_ret = allocate_cc_fixed(ptr->vm_start, ptr->vm_end, name);
				//get main program entry
				program_entry_addr = (long*)((long)&((Elf64_Ehdr *)0)->e_entry + ptr->vm_start);
				get_user(program_entry, program_entry_addr);
				PRINTK("[LKM:%d]entry=0x%lx\n", ts->pid, program_entry);
				set_checkpoint(ts, program_entry, ptr->vm_start, ptr->vm_end);
				has_handle_main_elf = 1;
			}
		}
#ifdef _VM
		if(ptr->vm_start==ld_bss_start){
			ld_regions[ld_region_num].region_start = ptr->vm_start;
			ld_regions[ld_region_num].region_len = ptr->vm_end - ptr->vm_start;					
			ld_regions[ld_region_num].prot = PROT_READ|PROT_WRITE;
			ld_regions[ld_region_num].flags = MAP_PRIVATE|MAP_DENYWRITE|MAP_FIXED;
			ld_regions[ld_region_num].off = 0;
			ld_regions[ld_region_num].file_ptr = NULL;
			ld_region_num++;
		}
#endif
		//find stack
		if((ptr->vm_flags&VM_STACK_FLAGS) == VM_STACK_FLAGS){
			map_addr = allocate_ss_fixed(ptr->vm_start, ptr->vm_end);
			//printk(KERN_ERR "shadow stack  = %08lx\n", map_addr);
		} 
		ptr = ptr->vm_next;
		if(ptr == NULL) break;
	} while (ptr != list);
	//unmap all ld.so
	//code cache map
	if(ld_regions[0].prot&PROT_EXEC){
		cc_ret = allocate_cc(ld_regions[0].region_len, LD_NAME);
		ld_offset = ld_regions[0].region_start - (cc_ret-CC_OFFSET);
	}else
		PRINTK("ld.so code cache find failed!\n");	
	//remap the ld.so
#ifdef _C10	
	ld_path = dentry_path_raw(ld_regions[0].file_ptr->f_path.dentry, buf, 256);
#endif
	for(index=0; index<ld_region_num; index++){
#ifdef _C10
		ld_fd = open_elf(ld_path);
		map_addr = orig_mmap(ld_regions[index].region_start-ld_offset, ld_regions[index].region_len,\
			ld_regions[index].prot, ld_regions[index].flags, ld_fd, ld_regions[index].off);
		close_elf(ld_fd);

		if(ld_regions[index].prot&PROT_WRITE){
			bk_buf = vmalloc(ld_regions[index].region_len);
			if(!bk_buf)
				PRINTK("error! failed to allocate memory!\n");
			copy_from_user(bk_buf, (void*)ld_regions[index].region_start, ld_regions[index].region_len);
			copy_to_user((void*)(ld_regions[index].region_start-ld_offset), bk_buf, ld_regions[index].region_len);
			vfree(bk_buf);
		}

		orig_munmap(ld_regions[index].region_start, ld_regions[index].region_len);
#else
		map_addr = vm_mmap(ld_regions[index].file_ptr, ld_regions[index].region_start-ld_offset,
			ld_regions[index].region_len, ld_regions[index].prot,
				ld_regions[index].flags, ld_regions[index].off);
		vm_munmap(ld_regions[index].region_start, ld_regions[index].region_len);
#endif	
	}
	//scan the aux in the stack and replace the base of interpreter
	replace_at_base_in_aux(ld_regions[0].region_start, ld_regions[index].region_start-ld_offset, ts);
	
	regs->ip -= ld_offset;
}

asmlinkage long intercept_munmap(unsigned long addr, size_t len)
{
	char monitor_idx = is_monitor_app(current->comm);
	if(monitor_idx!=0){
		free_mem(addr, len);	
	}

	return orig_munmap(addr, len);
}

asmlinkage long intercept_mmap(ulong addr, ulong len, ulong prot, ulong flags, ulong fd, ulong pgoff)
{
	long ret = 0;
	long cc_ret = 0;
	long ss_ret = 0;
	// insert the new element into the mapping table
	if(is_monitor_app(current->comm)){
		if((prot & PROT_EXEC) && ((int)fd)>0 && addr==0){			
			// 1.allocate code cache
				// 1.1 get x region file name
			struct file *fil = fget(fd);
			char* name = fil->f_path.dentry->d_iname;			
			cc_ret = allocate_cc(len, name);
			// 2.execute the original mmap, but the start is fixed with CC_OFFSET
			if(is_app_start(current))
				ret = orig_mmap(cc_ret-CC_OFFSET, len, prot, (flags&(~PROT_EXEC))|MAP_FIXED, fd, pgoff);
			else
				ret = orig_mmap(cc_ret-CC_OFFSET, len, prot, flags|MAP_FIXED, fd, pgoff);
			// 3.judge the code cache is allocate correctly
			if(cc_ret==0 || ret==0){
				PRINTK("[LKM]allocate code cache error!(mmap: cc_ret=%lx, ret=%lx)\n", cc_ret, ret);
				ret = orig_mmap(addr, len, prot, flags, fd, pgoff);
			}else{
			    //PRINTK("[ORG:%d]mmap(addr:%lx, len:%lx, prot:%d, flags:%d, fd:%d, off:%lx)= %lx\n", 
			    //	current->pid, addr, len, (int)prot, (int)flags, (int)fd, pgoff, ret);
			}
		}else if((flags&MAP_STACK) && addr==0){
			ret = orig_mmap(addr, len, prot, flags, fd, pgoff);
			ss_ret = allocate_ss_fixed(ret, ret+len);
			//PRINTK("[LKM]allocate child shadow stack (%lx)\n", ss_ret);
		}else
			ret = orig_mmap(addr, len, prot, flags, fd, pgoff);
	}else
		ret = orig_mmap(addr, len, prot, flags, fd, pgoff);
	
	return ret;
	
}

void mmap_last_rbbl_debug_page(ulong debug_base, ulong debug_size)
{
	long ret = orig_mmap(debug_base, debug_size, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_FIXED|MAP_ANONYMOUS, -1, 0);
	PRINTK("[LKM]mmap last rbbl debug page %lx-%lx\n", ret, ret+debug_size);
}
/*
static void setup_shuffle_process(const char __user* path, const char __user* const __user* cmdEnvp)
{
	int result = 0;
	int index = 0;
	char *cmdPath = "...";
	char* cmdArgv[]={cmdPath, (char*)path, NULL};
	result = call_usermodehelper(cmdPath, cmdArgv, (char**)cmdEnvp, UMH_NO_WAIT);
	//print cmd
	while(cmdArgv[index])
		PRINTK("%s ", cmdArgv[index++]);
		
	//print state
	if(result<0)
		PRINTK(" failed!\n");
	else
		PRINTK(" successed!\n");
}
*/
asmlinkage long intercept_execve(const char __user* filename, const char __user* const __user* argv,\
                    const char __user* const __user* envp)
{
	int ret = 0;
#ifdef TRACE_DEBUG
	long debug_buf_base = 0x50000000;
	long xchg_rsp_base = 0x101000;
	long *ptr = (long*)0x100000;
	long *ptr2 = (long*)0x100008;
#endif

	ret = orig_execve(filename, argv, envp);

	if(is_monitor_app(current->comm)){
		init_app_slot(current);
		remmap_interp_and_allocate_cc(current);
#if defined(LAST_RBBL_DEBUG) || defined(TRACE_DEBUG)	
		mmap_last_rbbl_debug_page(0x100000, 0x1000);
#endif
#ifdef TRACE_DEBUG
		allocate_trace_debug_buffer(0x50000000, 0x50000000);
		put_user(debug_buf_base, ptr);
		put_user(xchg_rsp_base, ptr2);
#endif		
	}
	return ret;
}

void send_exit_mesg_to_shuffle_process(struct task_struct *ts, int shuffle_pid)
{
	MESG_BAG msg = {P_PROCESS_IS_OUT, ts->pid, 0, {0}, CC_OFFSET, SS_OFFSET, GS_BASE, global_ss_type, "\0", "protected process is out!"};
	strcpy(msg.app_name, ts->comm);
	if(shuffle_pid!=0)
		nl_send_msg(shuffle_pid, msg);
	
	return ;
}

void send_ss_free_mesg_to_shuffle_process(struct task_struct *ts, char app_slot_idx, int shuffle_pid, long stack_len, char *shm_file)
{
	struct pt_regs *regs = task_pt_regs(ts);
	volatile char *start_flag = req_a_start_flag(app_slot_idx, ts->pid);
	MESG_BAG msg = {FREE_SS, ts->pid, regs->ip, {0}, stack_len, 0, 0, global_ss_type, "\0", "\0"};
	strcpy(msg.app_name, ts->comm);
	strcpy(msg.mesg, shm_file);
	
	if(shuffle_pid!=0){
		nl_send_msg(shuffle_pid, msg);
	
		*start_flag = 1;
		while(*start_flag){
			schedule();
		}
		regs->ip = get_shuffle_pc(app_slot_idx);

		free_a_start_flag(app_slot_idx, ts->pid);
	}
	
	return ;	
}

asmlinkage long intercept_exit_group(ulong error)
{	
	char *start_encode;
	char app_slot_idx;
	int shuffle_pid;
	char *stack_shm = NULL;
	long stack_len = 0;
	char monitor_idx = is_monitor_app(current->comm);
	if(monitor_idx!=0){
		start_encode = is_checkpoint(current, &app_slot_idx);
		
		if(start_encode){
			shuffle_pid = get_shuffle_pid(app_slot_idx);
			if(shuffle_pid==0){
				shuffle_pid = connect_one_shuffle(monitor_idx, app_slot_idx);
				set_shuffle_pid(app_slot_idx, shuffle_pid);
			}
			return set_program_start(current, start_encode, app_slot_idx);
		}else{
			PRINTK("[LKM]exit_group(%s)\n", current->comm);
			app_slot_idx = get_app_slot_idx(pid_vnr(task_pgrp(current)));
			if(app_slot_idx!=-1){
				shuffle_pid  = get_shuffle_pid(app_slot_idx);
				if(decrease_process_sum(current)!=0){//exit curr process, so we only need clear shadow stack
					stack_shm = get_stack_shm_info(current, &stack_len);
					if(stack_shm){
						send_ss_free_mesg_to_shuffle_process(current, app_slot_idx, shuffle_pid, stack_len, stack_shm);
						free_stack_info(pid_vnr(task_pgrp(current)), current->pid);
					}
					PRINTK("free_stack_info!\n");
				}else{//exit process group
					send_exit_mesg_to_shuffle_process(current, shuffle_pid);
					free_app_slot(current);
					PRINTK("free_app_slot!\n");
				}
			}
		}
	}

	return orig_exit_group(error);
}


#ifdef _C10
asmlinkage long intercept_sigaltstack(const stack_t __user *uss, stack_t __user *uoss, long arg2, long arg3, long arg4, long arg5, \
	long arg6, long arg7, struct pt_regs regs)
{
	char monitor_idx = is_monitor_app(current->comm);
	if(monitor_idx!=0){
		//PRINTK("%s had set signal stack by itself! We do not handle this case now!\n", current->comm);
		//mmap ss_size

		//allocate shadow stack

		//send message
	}
	return orig_sigaltstack(uss, uoss, arg2, arg3, arg4, arg5, arg6, arg7, regs);
}
#else
asmlinkage long intercept_sigaltstack(const struct sigaltstack __user *uss, struct sigaltstack __user *uoss)
{
	char monitor_idx = is_monitor_app(current->comm);
	if(monitor_idx!=0){
		//PRINTK("%s had set signal stack by itself! We do not handle this case now!\n", current->comm);
		//mmap ss_size

		//allocate shadow stack

		//send message
	}

	return orig_sigaltstack(uss, uoss);
}
#endif

void send_sigaction_mesg_to_shuffle_process(struct task_struct *ts, char app_slot_idx, int shuffle_pid, ulong handler_addr, ulong sigreturn_addr)
{
	struct pt_regs *regs = task_pt_regs(ts);
	volatile char *start_flag = req_a_start_flag(app_slot_idx, ts->pid);
	MESG_BAG msg = {SIGACTION_DETECTED, ts->pid, regs->ip, {0}, handler_addr, sigreturn_addr, \
		GS_BASE, global_ss_type, "\0", "handle sigaction handler and sigreturn!"};
	strcpy(msg.app_name, ts->comm);
	
	if(shuffle_pid!=0){
		nl_send_msg(shuffle_pid, msg);
		
		*start_flag = 1;
		while(*start_flag){
			schedule();
		}
		regs->ip = get_shuffle_pc(app_slot_idx);
		
		free_a_start_flag(app_slot_idx, ts->pid);
	}
	PRINTK("[%d]sigaction handled! NewPc=0x%lx\n", ts->pid, regs->ip);
	return ;
}

asmlinkage long intercept_rt_sigaction(int sig, struct sigaction __user *act, struct sigaction __user *oact, size_t sigsetsize)
{
	char monitor_idx = is_monitor_app(current->comm);
	int shuffle_pid = 0;
	char app_slot_idx = 0;
	long ret;
	int add_offset = 0;
	struct pt_regs *regs = task_pt_regs(current);
	if(monitor_idx!=0){
		PRINTK("[%d]%s had rt_sigaction sig=%d, pc = %lx\n", current->pid, current->comm, sig, regs->ip);
		
		//modify the handler address and sigreturn address
		app_slot_idx = get_app_slot_idx(pid_vnr(task_pgrp(current)));		
		shuffle_pid = get_shuffle_pid(app_slot_idx);

		if(shuffle_pid==0 && is_app_start(current)==0){
			shuffle_pid = connect_one_shuffle(monitor_idx, app_slot_idx);
			set_shuffle_pid(app_slot_idx, shuffle_pid);
		}
		
		if(shuffle_pid!=0 && act && act->sa_handler!=SIG_IGN && act->sa_handler!=SIG_DFL && act->sa_handler!=SIG_ERR){
			send_sigaction_mesg_to_shuffle_process(current, app_slot_idx, shuffle_pid, (ulong)act->sa_handler, (ulong)act->sa_restorer);
			act->sa_handler = (__sighandler_t)((long)act->sa_handler + CC_OFFSET);
			add_offset = 1;
		}
	}

	ret = orig_rt_sigaction(sig, act, oact, sigsetsize);

	if(add_offset==1)
		act->sa_handler = (__sighandler_t)((long)act->sa_handler - CC_OFFSET);

	return ret;
}

void send_ss_create_mesg_to_shuffle_process(struct task_struct *ts, char app_slot_idx, int shuffle_pid, long stack_len, char *shm_file)
{
	struct pt_regs *regs = task_pt_regs(ts);
	volatile char *start_flag = req_a_start_flag(app_slot_idx, ts->pid);
	MESG_BAG msg = {CREATE_SS, ts->pid, regs->ip, {0}, stack_len, 0, 0, global_ss_type, "\0", "\0"};
	strcpy(msg.app_name, ts->comm);
	strcpy(msg.mesg, shm_file);
	if(shuffle_pid!=0){
		nl_send_msg(shuffle_pid, msg);
		
		*start_flag = 1;
		while(*start_flag){
			schedule();
		}
		regs->ip = get_shuffle_pc(app_slot_idx);
		
		free_a_start_flag(app_slot_idx, ts->pid);
	}
	return ;	
}

#ifdef _C10
asmlinkage long intercept_clone(unsigned long clone_flags, unsigned long newsp, int __user * parent_tidptr,
	int __user * child_tidptr, unsigned long tls_val)

#else
asmlinkage long intercept_clone(unsigned long clone_flags, unsigned long newsp, int __user * parent_tidptr,
	int __user * child_tidptr, int tls_val)
#endif
{
	long ret = 0;
	long ss_ret = 0;
	long curr_stack_start = 0;
	long curr_stack_end = 0;
	void *bk_buf;
	long stack_len = 0;
	char app_slot_idx;
	int shuffle_pid = 0;
	char *shm_file = NULL;
	char monitor_idx = is_monitor_app(current->comm);
	if(monitor_idx!=0){
		app_slot_idx = get_app_slot_idx(pid_vnr(task_pgrp(current))); 
		if(app_slot_idx!=-1){
			shuffle_pid = get_shuffle_pid(app_slot_idx);			
			if(BITS_ARE_CLEAR(clone_flags, CLONE_VM)){				
				//increase num
				increase_process_sum(current);
			}

			if(shuffle_pid!=0 && BITS_ARE_CLEAR(clone_flags, CLONE_VM)){//fork process
				//need rerandomization
				close_rerandomization(current);//should use trap to trigger the exit_group to switch the page table(re-mmap code cache) in multi-processes
				rerandomization(current);
				get_ss_info(current, &curr_stack_start, &curr_stack_end);
				//backup the shadow stack information
				stack_len = curr_stack_end - curr_stack_start;
				bk_buf = vmalloc(stack_len);
				if(!bk_buf)
					PRINTK("error! failed to allocate memory!\n");
				copy_from_user(bk_buf, (void*)curr_stack_start, stack_len);
				//lock
				lock_app_slot();
			}
		}
	}

	ret = orig_clone(clone_flags, newsp, parent_tidptr, child_tidptr, tls_val);

	//child process had returned to ret_from_fork, so we only intercept the parent process

	if(monitor_idx!=0){	
		if(app_slot_idx!=-1 && shuffle_pid!=0 && BITS_ARE_CLEAR(clone_flags, CLONE_VM)){
			//modify the child shadow stack belonged info
			modify_stack_belong(current, current->pid, ret);
			unlock_app_slot();
			//free dead processes's shadow stack <handled for nginx server>
			free_dead_stack(current);
			//allocate the new shadow stack for parent process
			ss_ret = reallocate_ss(curr_stack_start, curr_stack_end);
			PRINTK("reallocate shadow stack %lx [%lx-%lx]\n", ss_ret, curr_stack_start, curr_stack_end);
			//initialize the shadow stack
			copy_to_user((void*)curr_stack_start, bk_buf, stack_len);
			vfree(bk_buf);
			shm_file = get_stack_shm_info(current, &stack_len);
			//send message to shuffle process
			send_ss_create_mesg_to_shuffle_process(current, app_slot_idx, shuffle_pid, stack_len, shm_file);
		}		
		PRINTK("[%d, %d] %s clone %ld\n", current->pid, pid_vnr(task_pgrp(current)), current->comm, ret);			
	}
	
	return ret;
}

asmlinkage long intercept_setsid(void)
{
	long ret;
	int old_pgid = pid_vnr(task_pgrp(current));
	ret = orig_setsid();
	if(is_monitor_app(current->comm)!=0){
		PRINTK("[%d, %d] %s in setsid\n", current->pid, pid_vnr(task_pgrp(current)), current->comm);
		create_new_sid(current, old_pgid);
		PRINTK("[%d, %d] %s out setsid\n", current->pid, pid_vnr(task_pgrp(current)), current->comm);
	}
	return ret;
}

