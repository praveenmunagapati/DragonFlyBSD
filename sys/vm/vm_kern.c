/*
 * Copyright (c) 1991, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * The Mach Operating System project at Carnegie-Mellon University.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	from: @(#)vm_kern.c	8.3 (Berkeley) 1/12/94
 *
 *
 * Copyright (c) 1987, 1990 Carnegie-Mellon University.
 * All rights reserved.
 *
 * Authors: Avadis Tevanian, Jr., Michael Wayne Young
 *
 * Permission to use, copy, modify and distribute this software and
 * its documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 *
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND
 * FOR ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 *
 * Carnegie Mellon requests users of this software to return to
 *
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 *
 * any improvements or extensions that they make and grant Carnegie the
 * rights to redistribute these changes.
 *
 * $FreeBSD: src/sys/vm/vm_kern.c,v 1.61.2.2 2002/03/12 18:25:26 tegge Exp $
 * $DragonFly: src/sys/vm/vm_kern.c,v 1.28 2007/01/02 04:52:31 dillon Exp $
 */

/*
 *	Kernel memory management.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/malloc.h>
#include <sys/kernel.h>
#include <sys/sysctl.h>

#include <vm/vm.h>
#include <vm/vm_param.h>
#include <sys/lock.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pageout.h>
#include <vm/vm_kern.h>
#include <vm/vm_extern.h>

struct vm_map kernel_map;
struct vm_map exec_map;
struct vm_map clean_map;
struct vm_map buffer_map;

/*
 *	kmem_alloc_pageable:
 *
 *	Allocate pageable memory to the kernel's address map.
 *	"map" must be kernel_map or a submap of kernel_map.
 */
vm_offset_t
kmem_alloc_pageable(vm_map_t map, vm_size_t size)
{
	vm_offset_t addr;
	int result;

	size = round_page(size);
	addr = vm_map_min(map);
	result = vm_map_find(map, NULL, (vm_offset_t) 0,
			     &addr, size,
			     TRUE, 
			     VM_MAPTYPE_NORMAL,
			     VM_PROT_ALL, VM_PROT_ALL,
			     0);
	if (result != KERN_SUCCESS) {
		return (0);
	}
	return (addr);
}

/*
 *	kmem_alloc_nofault:
 *
 *	Same as kmem_alloc_pageable, except that it create a nofault entry.
 */
vm_offset_t
kmem_alloc_nofault(vm_map_t map, vm_size_t size)
{
	vm_offset_t addr;
	int result;

	size = round_page(size);
	addr = vm_map_min(map);
	result = vm_map_find(map, NULL, (vm_offset_t) 0,
			     &addr, size,
			     TRUE,
			     VM_MAPTYPE_NORMAL,
			     VM_PROT_ALL, VM_PROT_ALL,
			     MAP_NOFAULT);
	if (result != KERN_SUCCESS) {
		return (0);
	}
	return (addr);
}

/*
 *	Allocate wired-down memory in the kernel's address map
 *	or a submap.
 */
vm_offset_t
kmem_alloc3(vm_map_t map, vm_size_t size, int kmflags)
{
	vm_offset_t addr;
	vm_offset_t i;
	int count;

	size = round_page(size);

	if (kmflags & KM_KRESERVE)
		count = vm_map_entry_kreserve(MAP_RESERVE_COUNT);
	else
		count = vm_map_entry_reserve(MAP_RESERVE_COUNT);

	/*
	 * Use the kernel object for wired-down kernel pages. Assume that no
	 * region of the kernel object is referenced more than once.
	 *
	 * Locate sufficient space in the map.  This will give us the final
	 * virtual address for the new memory, and thus will tell us the
	 * offset within the kernel map.
	 */
	vm_map_lock(map);
	if (vm_map_findspace(map, vm_map_min(map), size, 1, &addr)) {
		vm_map_unlock(map);
		if (kmflags & KM_KRESERVE)
			vm_map_entry_krelease(count);
		else
			vm_map_entry_release(count);
		return (0);
	}
	vm_object_reference(&kernel_object);
	vm_map_insert(map, &count,
		      &kernel_object, addr, addr, addr + size,
		      VM_MAPTYPE_NORMAL,
		      VM_PROT_ALL, VM_PROT_ALL,
		      0);
	vm_map_unlock(map);
	if (kmflags & KM_KRESERVE)
		vm_map_entry_krelease(count);
	else
		vm_map_entry_release(count);

	/*
	 * Guarantee that there are pages already in this object before
	 * calling vm_map_wire.  This is to prevent the following
	 * scenario:
	 *
	 * 1) Threads have swapped out, so that there is a pager for the
	 * kernel_object. 2) The kmsg zone is empty, and so we are
	 * kmem_allocing a new page for it. 3) vm_map_wire calls vm_fault;
	 * there is no page, but there is a pager, so we call
	 * pager_data_request.  But the kmsg zone is empty, so we must
	 * kmem_alloc. 4) goto 1 5) Even if the kmsg zone is not empty: when
	 * we get the data back from the pager, it will be (very stale)
	 * non-zero data.  kmem_alloc is defined to return zero-filled memory.
	 *
	 * We're intentionally not activating the pages we allocate to prevent a
	 * race with page-out.  vm_map_wire will wire the pages.
	 */

	for (i = 0; i < size; i += PAGE_SIZE) {
		vm_page_t mem;

		mem = vm_page_grab(&kernel_object, OFF_TO_IDX(addr + i),
			    VM_ALLOC_ZERO | VM_ALLOC_NORMAL | VM_ALLOC_RETRY);
		if ((mem->flags & PG_ZERO) == 0)
			vm_page_zero_fill(mem);
		mem->valid = VM_PAGE_BITS_ALL;
		vm_page_flag_clear(mem, PG_ZERO);
		vm_page_wakeup(mem);
	}

	/*
	 * And finally, mark the data as non-pageable.
	 */

	vm_map_wire(map, (vm_offset_t) addr, addr + size, kmflags);

	return (addr);
}

/*
 *	kmem_free:
 *
 *	Release a region of kernel virtual memory allocated
 *	with kmem_alloc, and return the physical pages
 *	associated with that region.
 *
 *	This routine may not block on kernel maps.
 */
void
kmem_free(vm_map_t map, vm_offset_t addr, vm_size_t size)
{
	vm_map_remove(map, trunc_page(addr), round_page(addr + size));
}

/*
 *	kmem_suballoc:
 *
 *	Used to break a system map into smaller maps, usually to reduce
 *	contention and to provide large KVA spaces for subsystems like the
 *	buffer cache.
 *
 *	parent		Map to take range from
 *	result	
 *	size		Size of range to find
 *	min, max	Returned endpoints of map
 *	pageable	Can the region be paged
 */
void
kmem_suballoc(vm_map_t parent, vm_map_t result,
	      vm_offset_t *min, vm_offset_t *max, vm_size_t size)
{
	int ret;

	size = round_page(size);

	*min = (vm_offset_t) vm_map_min(parent);
	ret = vm_map_find(parent, NULL, (vm_offset_t) 0,
			  min, size,
			  TRUE,
			  VM_MAPTYPE_UNSPECIFIED,
			  VM_PROT_ALL, VM_PROT_ALL,
			  0);
	if (ret != KERN_SUCCESS) {
		kprintf("kmem_suballoc: bad status return of %d.\n", ret);
		panic("kmem_suballoc");
	}
	*max = *min + size;
	pmap_reference(vm_map_pmap(parent));
	vm_map_init(result, *min, *max, vm_map_pmap(parent));
	if ((ret = vm_map_submap(parent, *min, *max, result)) != KERN_SUCCESS)
		panic("kmem_suballoc: unable to change range to submap");
}

/*
 *	kmem_alloc_wait:
 *
 *	Allocates pageable memory from a sub-map of the kernel.  If the submap
 *	has no room, the caller sleeps waiting for more memory in the submap.
 *
 *	This routine may block.
 */

vm_offset_t
kmem_alloc_wait(vm_map_t map, vm_size_t size)
{
	vm_offset_t addr;
	int count;

	size = round_page(size);

	count = vm_map_entry_reserve(MAP_RESERVE_COUNT);

	for (;;) {
		/*
		 * To make this work for more than one map, use the map's lock
		 * to lock out sleepers/wakers.
		 */
		vm_map_lock(map);
		if (vm_map_findspace(map, vm_map_min(map), size, 1, &addr) == 0)
			break;
		/* no space now; see if we can ever get space */
		if (vm_map_max(map) - vm_map_min(map) < size) {
			vm_map_entry_release(count);
			vm_map_unlock(map);
			return (0);
		}
		vm_map_unlock(map);
		tsleep(map, 0, "kmaw", 0);
	}
	vm_map_insert(map, &count,
		      NULL, (vm_offset_t) 0,
		      addr, addr + size,
		      VM_MAPTYPE_NORMAL,
		      VM_PROT_ALL, VM_PROT_ALL,
		      0);
	vm_map_unlock(map);
	vm_map_entry_release(count);
	return (addr);
}

/*
 *	kmem_free_wakeup:
 *
 *	Returns memory to a submap of the kernel, and wakes up any processes
 *	waiting for memory in that map.
 */
void
kmem_free_wakeup(vm_map_t map, vm_offset_t addr, vm_size_t size)
{
	int count;

	count = vm_map_entry_reserve(MAP_RESERVE_COUNT);
	vm_map_lock(map);
	vm_map_delete(map, trunc_page(addr), round_page(addr + size), &count);
	wakeup(map);
	vm_map_unlock(map);
	vm_map_entry_release(count);
}

/*
 * 	kmem_init:
 *
 *	Create the kernel_map and insert mappings to cover areas already
 *	allocated or reserved thus far.  That is, the area (KvaStart,start)
 *	and (end,KvaEnd) must be marked as allocated.
 *
 *	We could use a min_offset of 0 instead of KvaStart, but since the
 *	min_offset is not used for any calculations other then a bounds check
 *	it does not effect readability.  KvaStart is more appropriate.
 *
 *	Depend on the zalloc bootstrap cache to get our vm_map_entry_t.
 */
void
kmem_init(vm_offset_t start, vm_offset_t end)
{
	vm_map_t m;
	int count;

	m = vm_map_create(&kernel_map, &kernel_pmap, KvaStart, KvaEnd);
	vm_map_lock(m);
	/* N.B.: cannot use kgdb to debug, starting with this assignment ... */
	m->system_map = 1;
	count = vm_map_entry_reserve(MAP_RESERVE_COUNT);
	if (KvaStart != start) {
		vm_map_insert(m, &count, NULL, (vm_offset_t) 0,
			      KvaStart, start,
			      VM_MAPTYPE_NORMAL,
			      VM_PROT_ALL, VM_PROT_ALL,
			      0);
	}
	if (KvaEnd != end) {
		vm_map_insert(m, &count, NULL, (vm_offset_t) 0,
			      end, KvaEnd,
			      VM_MAPTYPE_NORMAL,
			      VM_PROT_ALL, VM_PROT_ALL,
			      0);
	}
	/* ... and ending with the completion of the above `insert' */
	vm_map_unlock(m);
	vm_map_entry_release(count);
}

static int
kvm_size(SYSCTL_HANDLER_ARGS)
{
	unsigned long ksize = KvaSize;

	return sysctl_handle_long(oidp, &ksize, 0, req);
}
SYSCTL_PROC(_vm, OID_AUTO, kvm_size, CTLTYPE_LONG|CTLFLAG_RD,
    0, 0, kvm_size, "IU", "Size of KVM");
 
static int
kvm_free(SYSCTL_HANDLER_ARGS)
{
	unsigned long kfree = virtual_end - kernel_vm_end;

	return sysctl_handle_long(oidp, &kfree, 0, req);
}
SYSCTL_PROC(_vm, OID_AUTO, kvm_free, CTLTYPE_LONG|CTLFLAG_RD,
    0, 0, kvm_free, "IU", "Amount of KVM free");

