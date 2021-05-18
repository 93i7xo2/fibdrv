CONFIG_MODULE_SIG = n
TARGET_MODULE := fibdrv

obj-m := $(TARGET_MODULE).o
$(TARGET_MODULE)-objs := fibdrv_mod.o bn.o
ccflags-y := -std=gnu99 -Wno-declaration-after-statement

KDIR := /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)

GIT_HOOKS := .git/hooks/applied

CPUID := $(shell nproc --all --ignore 1)
ISOLATED_CPU := $(shell cat /sys/devices/system/cpu/isolated)
ORIG_ASLR := $(shell cat /proc/sys/kernel/randomize_va_space)
ORIG_GOV := $(shell cat /sys/devices/system/cpu/cpu$(CPUID)/cpufreq/scaling_governor)
INTEL_BOOST_EXISTS := $(shell [ -e /sys/devices/system/cpu/intel_pstate/no_turbo ] && echo 1 || echo 0 )
BOOST_EXISTS := $(shell [ -e /sys/devices/system/cpu/cpufreq/boost ] && echo 1 || echo 0 )
ifeq ($(INTEL_BOOST_EXISTS), 1)
	ORIG_TURBO := $(shell cat /sys/devices/system/cpu/intel_pstate/no_turbo)
else ifeq ($(BOOST_EXISTS), 1)
	ORIG_TURBO := $(shell cat /sys/devices/system/cpu/cpufreq/boost)
endif

all: $(GIT_HOOKS) client
	$(MAKE) -C $(KDIR) M=$(PWD) modules

$(GIT_HOOKS):
	@scripts/install-git-hooks
	@echo

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	$(RM) client out
load:
	sudo insmod $(TARGET_MODULE).ko
unload:
	sudo rmmod $(TARGET_MODULE) || true >/dev/null

client: client.c
	$(CC) -o $@ $^

PRINTF = env printf
PASS_COLOR = \e[32;01m
NO_COLOR = \e[0m
pass = $(PRINTF) "$(PASS_COLOR)$1 Passed [-]$(NO_COLOR)\n"

check: all
	$(MAKE) unload
	$(MAKE) load
	sudo ./client > out
	$(MAKE) unload
	@diff -u out scripts/expected.txt && $(call pass)
	@scripts/verify.py

test: all
	$(MAKE) unload
	$(MAKE) load
	@python3 scripts/driver.py
	$(MAKE) unload

test2: all
ifneq ($(CPUID), $(ISOLATED_CPU))
	@echo "Isolated core must be the last of all cores."
	@exit 1
endif
	$(MAKE) unload
	$(MAKE) load
	sudo bash -c "echo 0 > /proc/sys/kernel/randomize_va_space"
	sudo bash -c "echo performance > /sys/devices/system/cpu/cpu$(CPUID)/cpufreq/scaling_governor"
ifeq ($(INTEL_BOOST_EXISTS), 1)
	sudo bash -c "echo 1 > /sys/devices/system/cpu/intel_pstate/no_turbo"
else ifeq ($(BOOST_EXISTS), 1)
	sudo bash -c "echo 0 > /sys/devices/system/cpu/cpufreq/boost"
endif
	@python3 scripts/driver.py
	sudo bash -c "echo $(ORIG_ASLR) > /proc/sys/kernel/randomize_va_space"
	sudo bash -c "echo $(ORIG_GOV) > /sys/devices/system/cpu/cpu$(CPUID)/cpufreq/scaling_governor"
ifeq ($(INTEL_BOOST_EXISTS), 1)
	sudo bash -c "echo $(ORIG_TURBO) > /sys/devices/system/cpu/intel_pstate/no_turbo"
else ifeq ($(BOOST_EXISTS), 1)
	sudo bash -c "echo $(ORIG_TURBO) > /sys/devices/system/cpu/cpufreq/boost"
endif
	$(MAKE) unload