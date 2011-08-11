/**
 * Copyright (c) 2010 Anup Patel.
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2, or (at your option)
 * any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 * @file vmm_scheduler.c
 * @version 1.0
 * @author Anup Patel (anup@brainfault.org)
 * @brief source file for hypervisor scheduler
 */

#include <vmm_error.h>
#include <vmm_string.h>
#include <vmm_heap.h>
#include <vmm_cpu.h>
#include <vmm_vcpu_irq.h>
#include <vmm_scheduler.h>

vmm_scheduler_ctrl_t sched;

void vmm_scheduler_next(vmm_user_regs_t * regs)
{
	s32 next;
	vmm_vcpu_t *cur_vcpu, *nxt_vcpu;

	/* Determine current vcpu */
	cur_vcpu = vmm_manager_vcpu(sched.vcpu_current);

	/* Determine the next ready vcpu to schedule */
	next = (cur_vcpu) ? cur_vcpu->num : -1;
	next = (next + 1) % vmm_manager_vcpu_count();
	nxt_vcpu = vmm_manager_vcpu(next);
	while ((nxt_vcpu->state != VMM_VCPU_STATE_READY) &&
		(next != sched.vcpu_current)) {
		next = (next + 1) % vmm_manager_vcpu_count();
		nxt_vcpu = vmm_manager_vcpu(next);
	}

	/* Do context switch between current and next vcpus */
	if (!cur_vcpu || (cur_vcpu->num != nxt_vcpu->num)) {
		if (cur_vcpu && (cur_vcpu->state & VMM_VCPU_STATE_SAVEABLE)) {
			if (cur_vcpu->state == VMM_VCPU_STATE_RUNNING) {
				cur_vcpu->state = VMM_VCPU_STATE_READY;
			}
			vmm_vcpu_regs_switch(cur_vcpu, nxt_vcpu, regs);
		} else {
			vmm_vcpu_regs_switch(NULL, nxt_vcpu, regs);
		}
	}

	if (nxt_vcpu) {
		nxt_vcpu->tick_pending = nxt_vcpu->tick_count;
		nxt_vcpu->state = VMM_VCPU_STATE_RUNNING;
		sched.vcpu_current = nxt_vcpu->num;
	}
}

void vmm_scheduler_timer_event(vmm_timer_event_t * event)
{
	vmm_vcpu_t * vcpu = vmm_manager_vcpu(sched.vcpu_current);
	if (vcpu) {
		if (!vcpu->preempt_count) {
			if (!vcpu->tick_pending) {
				vmm_scheduler_next(event->cpu_regs);
			} else {
				vcpu->tick_pending-=1;
				if (vcpu->tick_func && !vcpu->preempt_count) {
					vcpu->tick_func(event->cpu_regs, 
							vcpu->tick_pending);
				}
			}
		}
	} else {
		vmm_scheduler_next(event->cpu_regs);
	}
	vmm_timer_event_restart(event);
}

void vmm_scheduler_irq_process(vmm_user_regs_t * regs)
{
	vmm_vcpu_t * vcpu = NULL;

	/* Determine current vcpu */
	vcpu = vmm_manager_vcpu(sched.vcpu_current);
	if (!vcpu) {
		return;
	}

	/* Schedule next vcpu if state of 
	 * current vcpu is not RUNNING */
	if (vcpu->state != VMM_VCPU_STATE_RUNNING) {
		vmm_scheduler_next(regs);
		return;
	}

	/* VCPU irq processing */
	vmm_vcpu_irq_process(regs);

}

vmm_vcpu_t * vmm_scheduler_current_vcpu(void)
{
	irq_flags_t flags;
	vmm_vcpu_t * vcpu = NULL;
	flags = vmm_spin_lock_irqsave(&sched.lock);
	if (sched.vcpu_current != -1) {
		vcpu = vmm_manager_vcpu(sched.vcpu_current);
	}
	vmm_spin_unlock_irqrestore(&sched.lock, flags);
	return vcpu;
}

vmm_guest_t * vmm_scheduler_current_guest(void)
{
	vmm_vcpu_t *vcpu = vmm_scheduler_current_vcpu();
	if (vcpu) {
		return vcpu->guest;
	}
	return NULL;
}

void vmm_scheduler_preempt_disable(void)
{
	irq_flags_t flags;
	vmm_vcpu_t * vcpu = vmm_scheduler_current_vcpu();
	if (vcpu) {
		flags = vmm_cpu_irq_save();
		vcpu->preempt_count++;
		vmm_cpu_irq_restore(flags);
	}
}

void vmm_scheduler_preempt_enable(void)
{
	irq_flags_t flags;
	vmm_vcpu_t * vcpu = vmm_scheduler_current_vcpu();
	if (vcpu && vcpu->preempt_count) {
		flags = vmm_cpu_irq_save();
		vcpu->preempt_count--;
		vmm_cpu_irq_restore(flags);
	}
}

int vmm_scheduler_init(void)
{
	/* Reset the scheduler control structure */
	vmm_memset(&sched, 0, sizeof(sched));

	/* Initialize scheduling parameters. (Per Host CPU) */
	sched.vcpu_current = -1;

	/* Create timer event and start it. (Per Host CPU) */
	sched.ev = vmm_timer_event_create("sched", 
					  &vmm_scheduler_timer_event, 
					  NULL);
	if (!sched.ev) {
		return VMM_EFAIL;
	}
	vmm_timer_event_start(sched.ev, vmm_timer_tick_nsecs());

	return VMM_OK;
}
