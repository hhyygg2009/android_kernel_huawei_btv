# Kernel does not boot with instrumentation of tlb.c.
KCOV_INSTRUMENT_tlb.o	:= n

obj-y	:=  init.o init_$(BITS).o fault.o ioremap.o extable.o pageattr.o mmap.o \
	    pat.o pgtable.o physaddr.o gup.o setup_nx.o tlb.o

# Make sure __phys_addr has no stackprotector
nostackp := $(call cc-option, -fno-stack-protector)
CFLAGS_physaddr.o		:= $(nostackp)
CFLAGS_setup_nx.o		:= $(nostackp)

CFLAGS_fault.o := -I$(src)/../include/asm/trace

obj-$(CONFIG_X86_PAT)		+= pat_rbtree.o

obj-$(CONFIG_X86_32)		+= pgtable_32.o iomap_32.o

obj-$(CONFIG_HUGETLB_PAGE)	+= hugetlbpage.o
obj-$(CONFIG_X86_PTDUMP)	+= dump_pagetables.o

obj-$(CONFIG_HIGHMEM)		+= highmem_32.o

obj-$(CONFIG_KMEMCHECK)		+= kmemcheck/

KASAN_SANITIZE_kasan_init_$(BITS).o := n
obj-$(CONFIG_KASAN)		+= kasan_init_$(BITS).o

obj-$(CONFIG_MMIOTRACE)		+= mmiotrace.o
mmiotrace-y			:= kmmio.o pf_in.o mmio-mod.o
obj-$(CONFIG_MMIOTRACE_TEST)	+= testmmiotrace.o

obj-$(CONFIG_NUMA)		+= numa.o numa_$(BITS).o
obj-$(CONFIG_AMD_NUMA)		+= amdtopology.o
obj-$(CONFIG_ACPI_NUMA)		+= srat.o
obj-$(CONFIG_NUMA_EMU)		+= numa_emulation.o

obj-$(CONFIG_X86_INTEL_MPX)	+= mpx.o
