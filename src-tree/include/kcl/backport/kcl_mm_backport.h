/* SPDX-License-Identifier: GPL-2.0 */
#ifndef AMDKCL_MM_BACKPORT_H
#define AMDKCL_MM_BACKPORT_H
#include <kcl/kcl_mm.h>
#include <linux/sched/mm.h>
#include <linux/mm.h>

#ifndef HAVE_MMPUT_ASYNC
#define mmput_async _kcl_mmput_async
#endif

#ifdef get_user_pages_remote
#undef get_user_pages_remote
#endif
#ifdef get_user_pages
#undef get_user_pages
#endif

static inline
long kcl_get_user_pages_remote(struct task_struct *tsk, struct mm_struct *mm,
		unsigned long start, unsigned long nr_pages,
		unsigned int gup_flags, struct page **pages,
		struct vm_area_struct **vmas, int *locked)
{
#if defined(HAVE_GET_USER_PAGES_REMOTE_REMOVE_TASK_STRUCT)
	return get_user_pages_remote(mm, start, nr_pages, gup_flags, pages, vmas, locked);
#elif defined(HAVE_GET_USER_PAGES_REMOTE_LOCKED)
	return get_user_pages_remote(tsk, mm, start, nr_pages, gup_flags, pages, vmas, locked);
#elif defined(HAVE_GET_USER_PAGES_REMOTE_GUP_FLAGS)
	return get_user_pages_remote(tsk, mm, start, nr_pages, gup_flags, pages, vmas);
#elif defined(HAVE_GET_USER_PAGES_REMOTE_INTRODUCED)
	return get_user_pages_remote(tsk, mm, start, nr_pages, !!(gup_flags & FOLL_WRITE),
				     !!(gup_flags & FOLL_FORCE), pages, vmas);
#elif defined(HAVE_GET_USER_PAGES_REMOTE_REMOVE_VMAS)
	return get_user_pages_remote(mm, start, nr_pages, gup_flags, pages, locked);
#else
	return get_user_pages(tsk, mm, start, nr_pages, !!(gup_flags & FOLL_WRITE),
			      !!(gup_flags & FOLL_FORCE), pages, vmas);
#endif
}

#ifndef HAVE_GET_USER_PAGES_GUP_FLAGS
static inline
long _kcl_get_user_pages(unsigned long start, unsigned long nr_pages,
		unsigned int gup_flags, struct page **pages,
		struct vm_area_struct **vmas)
{
#if defined(HAVE_GET_USER_PAGES_6ARGS)
	return get_user_pages(start, nr_pages, !!(gup_flags & FOLL_WRITE),
			      !!(gup_flags & FOLL_FORCE), pages, vmas);
#elif defined(HAVE_GET_USER_PAGES_REMOVE_VMAS)
	return get_user_pages(start, nr_pages, gup_flags, pages);
#else
	return get_user_pages(current, current->mm, start, nr_pages, !!(gup_flags & FOLL_WRITE),
			      !!(gup_flags & FOLL_FORCE), pages, vmas);
#endif
}
#define get_user_pages _kcl_get_user_pages
#endif /* HAVE_GET_USER_PAGES_GUP_FLAGS */

/*
 * V17.5 Phase C: kcl shim for pin_user_pages_remote / unpin_user_pages.
 *
 * Mainline timeline:
 *   - 5.6+:  pin_user_pages_remote(mm, start, nr, flags, pages, vmas, locked)
 *   - 6.5+:  pin_user_pages_remote(mm, start, nr, flags, pages, locked)
 *            (vmas argument dropped, same commit as get_user_pages_remote)
 *
 * Customer target kernel is 6.14.14 (post-6.5). Build-host kernel may be
 * older; we dispatch on HAVE_GET_USER_PAGES_REMOTE_REMOVE_VMAS because
 * pin_user_pages_remote and get_user_pages_remote had vmas removed in
 * the same upstream commit.
 *
 * "pin" semantics guarantee that kernel reclaim, page migration, and
 * THP collapse skip the page — which is exactly what Phase C requires.
 *
 * Versions older than 5.6 are out of support for V17.5; we still leave
 * a degraded fallback (plain GUP without FOLL_LONGTERM) so older kernels
 * can build but should not be deployed for the cgroup-aware feature.
 */
static inline long
kcl_pin_user_pages_remote(struct mm_struct *mm, unsigned long start,
			  unsigned long nr_pages, unsigned int gup_flags,
			  struct page **pages)
{
#if defined(HAVE_GET_USER_PAGES_REMOTE_REMOVE_VMAS)
	return pin_user_pages_remote(mm, start, nr_pages, gup_flags,
				     pages, NULL);
#elif defined(HAVE_GET_USER_PAGES_REMOTE_REMOVE_TASK_STRUCT)
	return pin_user_pages_remote(mm, start, nr_pages, gup_flags,
				     pages, NULL, NULL);
#else
	/* Best-effort: plain GUP, no LONGTERM guarantee. Callers tolerate
	 * pin failure (Phase C falls back to legacy behavior on error). */
	return kcl_get_user_pages_remote(NULL, mm, start, nr_pages,
					 gup_flags & ~FOLL_LONGTERM,
					 pages, NULL, NULL);
#endif
}

static inline void kcl_unpin_user_pages(struct page **pages,
					unsigned long npages)
{
#if defined(HAVE_GET_USER_PAGES_REMOTE_REMOVE_VMAS) || \
    defined(HAVE_GET_USER_PAGES_REMOTE_REMOVE_TASK_STRUCT)
	unpin_user_pages(pages, npages);
#else
	unsigned long i;

	for (i = 0; i < npages; i++)
		if (pages[i])
			put_page(pages[i]);
#endif
}

#endif
