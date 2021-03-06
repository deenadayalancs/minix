/* This file contains the device dependent part of the drivers for the
 * following special files:
 *     /dev/ram		- RAM disk 
 *     /dev/mem		- absolute memory
 *     /dev/kmem	- kernel virtual memory
 *     /dev/null	- null device (data sink)
 *     /dev/boot	- boot device loaded from boot image 
 *     /dev/zero	- null byte stream generator
 *     /dev/imgrd	- boot image RAM disk
 *
 *  Changes:
 *	Apr 29, 2005	added null byte generator  (Jorrit N. Herder)
 *	Apr 09, 2005	added support for boot device  (Jorrit N. Herder)
 *	Jul 26, 2004	moved RAM driver to user-space  (Jorrit N. Herder)
 *	Apr 20, 1992	device dependent/independent split  (Kees J. Bot)
 */

#include <minix/drivers.h>
#include <minix/chardriver.h>
#include <minix/blockdriver.h>
#include <sys/ioc_memory.h>
#include <minix/ds.h>
#include <minix/vm.h>
#include <machine/param.h>
#include <sys/mman.h>
#include "kernel/const.h"
#include "kernel/config.h"
#include "kernel/type.h"

#include <machine/vm.h>

#include "local.h"

/* ramdisks (/dev/ram*) */
#define RAMDISKS     6

#define RAM_DEV_LAST (RAM_DEV_FIRST+RAMDISKS-1)

#define NR_DEVS            (7+RAMDISKS)	/* number of minor devices */

PRIVATE struct device m_geom[NR_DEVS];  /* base and size of each device */
PRIVATE vir_bytes m_vaddrs[NR_DEVS];
PRIVATE dev_t m_device;			/* current minor character device */

PRIVATE int openct[NR_DEVS];

FORWARD _PROTOTYPE( struct device *m_prepare, (dev_t device)		);
FORWARD _PROTOTYPE( int m_transfer, (endpoint_t endpt, int opcode,
	u64_t position, iovec_t *iov, unsigned int nr_req,
	endpoint_t user_endpt, unsigned int flags)			);
FORWARD _PROTOTYPE( int m_do_open, (message *m_ptr)			);
FORWARD _PROTOTYPE( int m_do_close, (message *m_ptr)			);

FORWARD _PROTOTYPE( struct device *m_block_part, (dev_t minor)		);
FORWARD _PROTOTYPE( int m_block_transfer, (dev_t minor, int do_write,
	u64_t position, endpoint_t endpt, iovec_t *iov,
	unsigned int nr_req, int flags)					);
FORWARD _PROTOTYPE( int m_block_open, (dev_t minor, int access)		);
FORWARD _PROTOTYPE( int m_block_close, (dev_t minor)			);
FORWARD _PROTOTYPE( int m_block_ioctl, (dev_t minor,
	unsigned int request, endpoint_t endpt, cp_grant_id_t grant)	);

/* Entry points to the CHARACTER part of this driver. */
PRIVATE struct chardriver m_cdtab = {
  m_do_open,	/* open or mount */
  m_do_close,	/* nothing on a close */
  nop_ioctl,	/* no I/O control */
  m_prepare,	/* prepare for I/O on a given minor device */
  m_transfer,	/* do the I/O */
  nop_cleanup,	/* no need to clean up */
  nop_alarm,	/* no alarms */
  nop_cancel,	/* no blocking operations */
  nop_select,	/* select not supported */
  NULL		/* other messages not supported */
};

/* Entry points to the BLOCK part of this driver. */
PRIVATE struct blockdriver m_bdtab = {
  BLOCKDRIVER_TYPE_DISK,/* handle partition requests */
  m_block_open,		/* open or mount */
  m_block_close,	/* nothing on a close */
  m_block_transfer,	/* do the I/O */
  m_block_ioctl,	/* ram disk I/O control */
  NULL,			/* no need to clean up */
  m_block_part,		/* return partition information */
  NULL,			/* no geometry */
  NULL,			/* no interrupt processing */
  NULL,			/* no alarm processing */
  NULL,			/* no processing of other messages */
  NULL			/* no threading support */
};

/* Buffer for the /dev/zero null byte feed. */
#define ZERO_BUF_SIZE			1024
PRIVATE char dev_zero[ZERO_BUF_SIZE];

#define click_to_round_k(n) \
	((unsigned) ((((unsigned long) (n) << CLICK_SHIFT) + 512) / 1024))

/* SEF functions and variables. */
FORWARD _PROTOTYPE( void sef_local_startup, (void) );
FORWARD _PROTOTYPE( int sef_cb_init_fresh, (int type, sef_init_info_t *info) );

/*===========================================================================*
 *				   main 				     *
 *===========================================================================*/
PUBLIC int main(void)
{
  message msg;
  int r, ipc_status;

  /* SEF local startup. */
  sef_local_startup();

  /* The receive loop. */
  for (;;) {
	if ((r = driver_receive(ANY, &msg, &ipc_status)) != OK)
		panic("memory: driver_receive failed (%d)", r);

	if (IS_BDEV_RQ(msg.m_type))
		blockdriver_process(&m_bdtab, &msg, ipc_status);
	else
		chardriver_process(&m_cdtab, CHARDRIVER_SYNC, &msg,
			ipc_status);
  }

  return(OK);
}

/*===========================================================================*
 *			       sef_local_startup			     *
 *===========================================================================*/
PRIVATE void sef_local_startup()
{
  /* Register init callbacks. */
  sef_setcb_init_fresh(sef_cb_init_fresh);
  sef_setcb_init_lu(sef_cb_init_fresh);
  sef_setcb_init_restart(sef_cb_init_fresh);

  /* Register live update callbacks. */
  sef_setcb_lu_prepare(sef_cb_lu_prepare_always_ready);
  sef_setcb_lu_state_isvalid(sef_cb_lu_state_isvalid_standard);

  /* Let SEF perform startup. */
  sef_startup();
}

/*===========================================================================*
 *		            sef_cb_init_fresh                                *
 *===========================================================================*/
PRIVATE int sef_cb_init_fresh(int UNUSED(type), sef_init_info_t *UNUSED(info))
{
/* Initialize the memory driver. */
  int i;
#if 0
  struct kinfo kinfo;		/* kernel information */
  int s;

  if (OK != (s=sys_getkinfo(&kinfo))) {
      panic("Couldn't get kernel information: %d", s);
  }

  /* Map in kernel memory for /dev/kmem. */
  m_geom[KMEM_DEV].dv_base = cvul64(kinfo.kmem_base);
  m_geom[KMEM_DEV].dv_size = cvul64(kinfo.kmem_size);
  if((m_vaddrs[KMEM_DEV] = vm_map_phys(SELF, (void *) kinfo.kmem_base,
	kinfo.kmem_size)) == MAP_FAILED) {
	printf("MEM: Couldn't map in /dev/kmem.");
  }
#endif

  /* Ramdisk image built into the memory driver */
  m_geom[IMGRD_DEV].dv_base= cvul64(0);
  m_geom[IMGRD_DEV].dv_size= cvul64(imgrd_size);
  m_vaddrs[IMGRD_DEV] = (vir_bytes) imgrd;

  /* Initialize /dev/zero. Simply write zeros into the buffer. */
  for (i=0; i<ZERO_BUF_SIZE; i++) {
       dev_zero[i] = '\0';
  }

  for(i = 0; i < NR_DEVS; i++)
	openct[i] = 0;

  /* Set up memory range for /dev/mem. */
  m_geom[MEM_DEV].dv_base = cvul64(0);
  m_geom[MEM_DEV].dv_size = cvul64(0xffffffff);

  m_vaddrs[MEM_DEV] = (vir_bytes) MAP_FAILED; /* we are not mapping this in. */

  return(OK);
}

/*===========================================================================*
 *				m_is_block				     *
 *===========================================================================*/
PRIVATE int m_is_block(dev_t minor)
{
/* Return TRUE iff the given minor device number is for a block device. */

  switch (minor) {
  case MEM_DEV:
  case KMEM_DEV:
  case NULL_DEV:
  case ZERO_DEV:
	return FALSE;

  default:
	return TRUE;
  }
}

/*===========================================================================*
 *				m_prepare				     *
 *===========================================================================*/
PRIVATE struct device *m_prepare(dev_t device)
{
/* Prepare for I/O on a device: check if the minor device number is ok. */
  if (device >= NR_DEVS || m_is_block(device)) return(NULL);
  m_device = device;

  return(&m_geom[device]);
}

/*===========================================================================*
 *				m_transfer				     *
 *===========================================================================*/
PRIVATE int m_transfer(
  endpoint_t endpt,		/* endpoint of grant owner */
  int opcode,			/* DEV_GATHER_S or DEV_SCATTER_S */
  u64_t pos64,			/* offset on device to read or write */
  iovec_t *iov,			/* pointer to read or write request vector */
  unsigned int nr_req,		/* length of request vector */
  endpoint_t UNUSED(user_endpt),/* endpoint of user process */
  unsigned int UNUSED(flags)
)
{
/* Read or write one the driver's character devices. */
  unsigned count, left, chunk;
  vir_bytes vir_offset = 0;
  struct device *dv;
  unsigned long dv_size;
  int s, r;
  off_t position;
  cp_grant_id_t grant;
  vir_bytes dev_vaddr;

  /* ZERO_DEV and NULL_DEV are infinite in size. */
  if (m_device != ZERO_DEV && m_device != NULL_DEV && ex64hi(pos64) != 0)
	return OK;	/* Beyond EOF */
  position= cv64ul(pos64);

  /* Get minor device number and check for /dev/null. */
  dv = &m_geom[m_device];
  dv_size = cv64ul(dv->dv_size);
  dev_vaddr = m_vaddrs[m_device];

  while (nr_req > 0) {

	/* How much to transfer and where to / from. */
	count = iov->iov_size;
	grant = (cp_grant_id_t) iov->iov_addr;

	switch (m_device) {

	/* No copying; ignore request. */
	case NULL_DEV:
	    if (opcode == DEV_GATHER_S) return(OK);	/* always at EOF */
	    break;

	/* Virtual copying. For kernel memory. */
	default:
	case KMEM_DEV:
	    if(!dev_vaddr || dev_vaddr == (vir_bytes) MAP_FAILED) {
		printf("MEM: dev %d not initialized\n", m_device);
		return EIO;
	    }
	    if (position >= dv_size) return(OK);	/* check for EOF */
	    if (position + count > dv_size) count = dv_size - position;
	    if (opcode == DEV_GATHER_S) {	/* copy actual data */
	        r=sys_safecopyto(endpt, grant, vir_offset,
		  dev_vaddr + position, count, D);
	    } else {
	        r=sys_safecopyfrom(endpt, grant, vir_offset,
		  dev_vaddr + position, count, D);
	    }
	    if(r != OK) {
              panic("I/O copy failed: %d", r);
	    }
	    break;

	/* Physical copying. Only used to access entire memory.
	 * Transfer one 'page window' at a time.
	 */
	case MEM_DEV:
	{
	    u32_t pagestart, page_off;
	    static u32_t pagestart_mapped;
	    static int any_mapped = 0;
	    static char *vaddr;
	    int r;
	    u32_t subcount;
	    phys_bytes mem_phys;

	    if (position >= dv_size)
		return(OK);	/* check for EOF */
	    if (position + count > dv_size)
		count = dv_size - position;
	    mem_phys = position;

	    page_off = mem_phys % I386_PAGE_SIZE;
	    pagestart = mem_phys - page_off; 

	    /* All memory to the map call has to be page-aligned.
	     * Don't have to map same page over and over.
	     */
	    if(!any_mapped || pagestart_mapped != pagestart) {
	     if(any_mapped) {
		if(vm_unmap_phys(SELF, vaddr, I386_PAGE_SIZE) != OK)
			panic("vm_unmap_phys failed");
		any_mapped = 0;
	     }
	     vaddr = vm_map_phys(SELF, (void *) pagestart, I386_PAGE_SIZE);
	     if(vaddr == MAP_FAILED) 
		r = ENOMEM;
	     else
		r = OK;
	     if(r != OK) {
		printf("memory: vm_map_phys failed\n");
		return r;
	     }
	     any_mapped = 1;
	     pagestart_mapped = pagestart;
	   }

	    /* how much to be done within this page. */
	    subcount = I386_PAGE_SIZE-page_off;
	    if(subcount > count)
		subcount = count;

	    if (opcode == DEV_GATHER_S) {			/* copy data */
	           s=sys_safecopyto(endpt, grant,
		       vir_offset, (vir_bytes) vaddr+page_off, subcount, D);
	    } else {
	           s=sys_safecopyfrom(endpt, grant,
		       vir_offset, (vir_bytes) vaddr+page_off, subcount, D);
	    }
	    if(s != OK)
		return s;
	    count = subcount;
	    break;
	}

	/* Null byte stream generator. */
	case ZERO_DEV:
	    if (opcode == DEV_GATHER_S) {
		size_t suboffset = 0;
	        left = count;
		while (left > 0) {
		    chunk = (left > ZERO_BUF_SIZE) ? ZERO_BUF_SIZE : left;
	             s=sys_safecopyto(endpt, grant,
		       vir_offset+suboffset, (vir_bytes) dev_zero, chunk, D);
		    if(s != OK)
		        return s;
		    left -= chunk;
	            suboffset += chunk;
		}
	    }
	    break;

	}

	/* Book the number of bytes transferred. */
	position += count;
	vir_offset += count;
	if ((iov->iov_size -= count) == 0) { iov++; nr_req--; vir_offset = 0; }

  }
  return(OK);
}

/*===========================================================================*
 *				m_do_open				     *
 *===========================================================================*/
PRIVATE int m_do_open(message *m_ptr)
{
/* Open a memory character device. */
  int r;

/* Check device number on open. */
  if (m_prepare(m_ptr->DEVICE) == NULL) return(ENXIO);
  if (m_device == MEM_DEV)
  {
	r = sys_enable_iop(m_ptr->USER_ENDPT);
	if (r != OK)
	{
		printf("m_do_open: sys_enable_iop failed for %d: %d\n",
			m_ptr->USER_ENDPT, r);
		return r;
	}
  }

  openct[m_device]++;

  return(OK);
}

/*===========================================================================*
 *				m_do_close				     *
 *===========================================================================*/
PRIVATE int m_do_close(message *m_ptr)
{
/* Close a memory character device. */
  if (m_prepare(m_ptr->DEVICE) == NULL) return(ENXIO);

  if(openct[m_device] < 1) {
	printf("MEMORY: closing unopened device %d\n", m_device);
	return(EINVAL);
  }
  openct[m_device]--;

  return(OK);
}

/*===========================================================================*
 *				m_block_part				     *
 *===========================================================================*/
PRIVATE struct device *m_block_part(dev_t minor)
{
/* Prepare for I/O on a device: check if the minor device number is ok. */
  if (minor >= NR_DEVS || !m_is_block(minor)) return(NULL);

  return(&m_geom[minor]);
}

/*===========================================================================*
 *				m_block_transfer			     *
 *===========================================================================*/
PRIVATE int m_block_transfer(
  dev_t minor,			/* minor device number */
  int do_write,			/* read or write? */
  u64_t pos64,			/* offset on device to read or write */
  endpoint_t endpt,		/* process doing the request */
  iovec_t *iov,			/* pointer to read or write request vector */
  unsigned int nr_req,		/* length of request vector */
  int UNUSED(flags)		/* transfer flags */
)
{
/* Read or write one the driver's block devices. */
  unsigned count;
  vir_bytes vir_offset = 0;
  struct device *dv;
  unsigned long dv_size;
  int r;
  off_t position;
  vir_bytes dev_vaddr;
  cp_grant_id_t grant;
  ssize_t total = 0;

  /* Get minor device information. */
  if ((dv = m_block_part(minor)) == NULL) return(ENXIO);
  dv_size = cv64ul(dv->dv_size);
  dev_vaddr = m_vaddrs[minor];

  if (ex64hi(pos64) != 0)
	return OK;	/* Beyond EOF */
  position= cv64ul(pos64);

  while (nr_req > 0) {

	/* How much to transfer and where to / from. */
	count = iov->iov_size;
	grant = (cp_grant_id_t) iov->iov_addr;

	/* Virtual copying. For RAM disks and internal FS. */
	if(!dev_vaddr || dev_vaddr == (vir_bytes) MAP_FAILED) {
		printf("MEM: dev %d not initialized\n", minor);
		return EIO;
	}
	if (position >= dv_size) return(total);	/* check for EOF */
	if (position + count > dv_size) count = dv_size - position;
	if (!do_write) {	/* copy actual data */
	        r=sys_safecopyto(endpt, grant, vir_offset,
		  dev_vaddr + position, count, D);
	} else {
	        r=sys_safecopyfrom(endpt, grant, vir_offset,
		  dev_vaddr + position, count, D);
	}
	if(r != OK) {
		panic("I/O copy failed: %d", r);
	}

	/* Book the number of bytes transferred. */
	position += count;
	vir_offset += count;
	total += count;
	if ((iov->iov_size -= count) == 0) { iov++; nr_req--; vir_offset = 0; }

  }
  return(total);
}

/*===========================================================================*
 *				m_block_open				     *
 *===========================================================================*/
PRIVATE int m_block_open(dev_t minor, int UNUSED(access))
{
/* Open a memory block device. */
  if (m_block_part(minor) == NULL) return(ENXIO);

  openct[minor]++;

  return(OK);
}

/*===========================================================================*
 *				m_block_close				     *
 *===========================================================================*/
PRIVATE int m_block_close(dev_t minor)
{
/* Close a memory block device. */
  if (m_block_part(minor) == NULL) return(ENXIO);

  if(openct[minor] < 1) {
	printf("MEMORY: closing unopened device %d\n", minor);
	return(EINVAL);
  }
  openct[minor]--;

#if 0
  /* Special case: free initial ramdisk after it's been unmounted once. */
  if(minor == IMGRD_DEV && openct[minor] == 0 && m_vaddrs[IMGRD_DEV]) {
	vir_bytes vaddr, vlen;
	vaddr = m_vaddrs[IMGRD_DEV];
	vlen = imgrd_size;
	/* Align `inwards' so as to not unmap more than the initial
	 * ramdisk image.
	 */
	if(vaddr % PAGE_SIZE) {
		vir_bytes o = PAGE_SIZE - (vaddr % PAGE_SIZE);
		vlen -= o;
		vaddr += o;
	}
	if(vlen % PAGE_SIZE) {
		vlen -= vlen % PAGE_SIZE;
	}
	minix_munmap((void *) vaddr, vlen);
	m_geom[IMGRD_DEV].dv_base= cvul64(0);
	m_geom[IMGRD_DEV].dv_size= cvul64(0);
	m_vaddrs[IMGRD_DEV] = 0;
  }
#endif

  return(OK);
}

/*===========================================================================*
 *				m_block_ioctl				     *
 *===========================================================================*/
PRIVATE int m_block_ioctl(dev_t minor, unsigned int request, endpoint_t endpt,
	cp_grant_id_t grant)
{
/* I/O controls for the block devices of the memory driver. Currently there is
 * one I/O control specific to the memory driver:
 * - MIOCRAMSIZE: to set the size of the RAM disk.
 */
  struct device *dv;
  u32_t ramdev_size;
  int s;
  void *mem;

  if (request != MIOCRAMSIZE)
	return EINVAL;

  /* Someone wants to create a new RAM disk with the given size.
   * A ramdisk can be created only once, and only on RAM disk device.
   */
  if ((dv = m_block_part(minor)) == NULL) return ENXIO;
  if((minor < RAM_DEV_FIRST || minor > RAM_DEV_LAST) && minor != RAM_DEV_OLD) {
	printf("MEM: MIOCRAMSIZE: %d not a ramdisk\n", minor);
	return EINVAL;
  }

  /* Get request structure */
  s= sys_safecopyfrom(endpt, grant, 0, (vir_bytes)&ramdev_size,
	sizeof(ramdev_size), D);
  if (s != OK)
	return s;
  if(m_vaddrs[minor] && !cmp64(dv->dv_size, cvul64(ramdev_size))) {
	return(OK);
  }
  /* openct is 1 for the ioctl(). */
  if(openct[minor] != 1) {
	printf("MEM: MIOCRAMSIZE: %d in use (count %d)\n",
		minor, openct[minor]);
	return(EBUSY);
  }
  if(m_vaddrs[minor]) {
	u32_t size;
	if(ex64hi(dv->dv_size)) {
		panic("huge old ramdisk");
	}
	size = ex64lo(dv->dv_size);
	minix_munmap((void *) m_vaddrs[minor], size);
	m_vaddrs[minor] = (vir_bytes) NULL;
  }

#if DEBUG
  printf("MEM:%d: allocating ramdisk of size 0x%x\n", minor, ramdev_size);
#endif

  /* Try to allocate a piece of memory for the RAM disk. */
  if((mem = minix_mmap(NULL, ramdev_size, PROT_READ|PROT_WRITE,
		MAP_PREALLOC|MAP_ANON, -1, 0)) == MAP_FAILED) {
	printf("MEM: failed to get memory for ramdisk\n");
	return(ENOMEM);
  }

  m_vaddrs[minor] = (vir_bytes) mem;

  dv->dv_size = cvul64(ramdev_size);

  return(OK);
}
