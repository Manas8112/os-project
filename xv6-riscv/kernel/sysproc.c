#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

// Expose tracing_enabled defined in kernel/proc.c[cite: 2]
extern int tracing_enabled;
extern struct spinlock wait_lock;

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0; // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if (t == SBRK_EAGER || n < 0) {
    if (growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if (addr + n < addr)
      return -1;
    if (addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if (n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (killed(myproc())) {
      release(&tickslock);
      return -1;
    }
    sleep_prepare(&ticks);
    release(&tickslock);
    sleep();
    acquire(&tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64
sys_trace(void)
{
  int on;
  argint(0, &on);
  tracing_enabled = on;
  return 0;
}

uint64
sys_getforkcount(void)
{
  return myproc()->forkcount;
}

uint64
sys_getancestors(void)
{
  uint64 pids;
  int max;
  int count = 0;
  int kbuf[16];

  argaddr(0, &pids);
  argint(1, &max);

  if (max <= 0) return 0;
  if (max > 16) max = 16;

  struct proc *p = myproc();

  // Lock wait_lock to safely traverse parent pointers across CPUs[cite: 2]
  acquire(&wait_lock);
  struct proc *curr = p->parent;
  while (curr != 0 && curr->pid >= 1 && count < max) {
    kbuf[count++] = curr->pid;
    curr = curr->parent;
  }
  release(&wait_lock);

  // Safely copy collected ancestor PIDs to user memory space[cite: 2]
  if (copyout(p->pagetable, p->sz, pids, (char*)kbuf, count * sizeof(int)) < 0)
    return -1;

  return count;
}

uint64
sys_cowstats(void)
{
  uint64 fault_addr;
  uint64 copy_addr;
  struct proc *p = myproc();

  int faults = p->cow_faults;
  int copies = p->cow_copies;

  argaddr(0, &fault_addr);
  argaddr(1, &copy_addr);

  if (copyout(p->pagetable, p->sz, fault_addr,
              (char *)&faults, sizeof(int)) < 0)
    return -1;

  if (copyout(p->pagetable, p->sz, copy_addr,
              (char *)&copies, sizeof(int)) < 0)
    return -1;

  return 0;
}

uint64
sys_freemem(void)
{
  return getfreemem();
}
