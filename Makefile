# SPDX-License-Identifier: GPL-2.0
# ============================================================================
# Linux 内核顶层 Makefile
# ============================================================================
# 这是 Linux 内核构建系统的入口点，采用递归 make 方式组织构建。
# 整体架构：
#   1. 第一遍 make：设置变量、检测输出目录、必要时递归调用自身
#   2. 第二遍 make：真正的构建逻辑，包含配置目标和构建目标
#
# 支持的构建模式：
#   - 源码目录内构建：在源码树下直接 make
#   - 源码目录外构建：make O=/path/to/build  将输出文件放到独立目录
#   - 外部模块构建：  make M=/path/to/module 编译内核树外的模块
#
# 主要目标分类：
#   - *config 目标：内核配置（menuconfig, oldconfig, defconfig 等）
#   - 构建目标：   vmlinux, modules, dtbs 等
#   - 清理目标：   clean, mrproper, distclean
#   - 安装目标：   install, modules_install, headers_install
# ============================================================================

# 内核版本号：Makefile 中定义为 7.1.0-rc3
# 最终的内核发布版本字符串由 scripts/setlocalversion 脚本生成
# 主版本号、补丁级别（次版本号）、子级别
VERSION = 7
PATCHLEVEL = 1
SUBLEVEL = 0
# 额外版本字符串（-rc3 表示第 3 个候选发布版）
EXTRAVERSION = -rc3
# 此版本的内核代号（每个版本有独特的代号）
NAME = Baby Opossum Posse

# *DOCUMENTATION*
# To see a list of typical targets execute "make help"
# More info can be located in ./README
# Comments in this file are targeted only to the developer, do not
# expect to learn how to build the kernel reading this file.

# ============================================================================
# 环境检查：要求 GNU Make >= 4.0
# output-sync 功能是 GNU Make 4.0 引入的，用于同步并行构建的输出
# ============================================================================
ifeq ($(filter output-sync,$(.FEATURES)),)
$(error GNU Make >= 4.0 is required. Your Make version is $(MAKE_VERSION))
endif

# 禁止用户直接使用以 '__' 为前缀的内部目标
$(if $(filter __%, $(MAKECMDGOALS)), \
	$(error targets prefixed with '__' are only for internal use))

# 默认目标：当命令行没有指定目标时，构建 __all
PHONY := __all
__all:

# ============================================================================
# 递归构建说明
# ============================================================================
# 内核使用递归 make 构建（即 make 会调用子目录中的 Makefile）。
#
# 核心原理：
#   每个子目录（如 kernel/, mm/, drivers/）包含一个 Kbuild 或 Makefile，
#   该文件定义了该目录中要编译的目标文件列表（obj-y / obj-m）。
#   顶层 Makefile 通过 $(build)=<dir> 宏递归调用 scripts/Makefile.build，
#   在子目录中执行编译。
#
# 最重要的原则：子 Makefile 只应修改自己目录中的文件。
# 这保证了并行构建的正确性（make -j），因为每个目录的文件是独立的。
#
# 当某个目录依赖另一个目录的文件时（例如链接 built-in.a 为 vmlinux 时），
# 会先在那个目录中递归调用 make，确保所有依赖都是最新的。
#
# 需要全局影响的修改（如生成头文件、配置同步等）被分离出来，
# 在递归下降之前完成，这些统一列在 prepare 规则中。
#
# 构建顺序：
#   1. prepare（准备阶段：生成头文件、配置同步、构建辅助工具等）
#   2. 递归下降到各子目录（由 scripts/Makefile.build 驱动）
#      - 每个子目录的 built-in.a 被创建（该目录所有内置目标文件的归档）
#      - 每个子目录的 .tmp_%.o 等临时规则文件被生成
#   3. 链接 vmlinux（由 scripts/Makefile.vmlinux 和 scripts/link-vmlinux.sh 完成）
#      - 首先将所有的 built-in.a 合并为 vmlinux.a
#      - 然后将 vmlinux.a 与 lib.a 链接为 vmlinux.o
#      - 最后通过链接脚本链接为最终的 vmlinux ELF 镜像

this-makefile := $(lastword $(MAKEFILE_LIST))
abs_srctree := $(realpath $(dir $(this-makefile)))
abs_output := $(CURDIR)

# ============================================================================
# 第一遍 make：设置环境和变量
# sub_make_done 用于区分是第一遍还是第二遍 make
# 第一遍设置好所有变量后，会递归调用自身进入第二遍
# ============================================================================
ifneq ($(sub_make_done),1)

# 禁用 make 的内置规则和变量（提高性能，避免不可预测的行为）
MAKEFLAGS += -rR

# 避免字符集依赖问题：强制使用 C 语言环境
unexport LC_ALL
LC_COLLATE=C
LC_NUMERIC=C
export LC_COLLATE LC_NUMERIC

# 避免 shell 环境变量的干扰（GREP_OPTIONS 会改变 grep 的输出格式）
unexport GREP_OPTIONS

# ============================================================================
# 构建输出美化（Beautify output）
# ============================================================================
# Kbuild 中的构建命令通常以 "cmd_" 开头。
# 可以定义对应的 "quiet_cmd_*"，用于输出简短日志。
# 如果没有定义 quiet_cmd_*，默认不输出任何日志。
#
# 例如：
#    quiet_cmd_depmod = DEPMOD  $(MODLIB)    # 简短输出
#          cmd_depmod = $(srctree)/scripts/depmod.sh ...  # 实际命令
#
# 简单的方式是在命令前加 $(Q)：
#    $(Q)$(MAKE) $(build)=scripts/basic
#
# KBUILD_VERBOSE 控制输出详细程度：
#   V=0（默认）：显示简短日志
#   V=1：       显示完整的编译命令
#   V=2：       显示重新编译的原因

ifeq ("$(origin V)", "command line")  # 如果 V 是从命令行指定的
  KBUILD_VERBOSE = $(V)
endif

# quiet 用于选择简短输出前缀（quiet_ 或 silent_），Q 用于抑制命令回显（@）
quiet = quiet_
Q = @

ifneq ($(findstring 1, $(KBUILD_VERBOSE)),)
  quiet =
  Q =
endif

# make -s（静默模式）：完全抑制命令输出
ifneq ($(findstring s,$(firstword -$(MAKEFLAGS))),)
quiet=silent_
override KBUILD_VERBOSE :=
endif

export quiet Q KBUILD_VERBOSE

# ============================================================================
# 源代码检查器（默认使用 sparse）
# ============================================================================
# C=1：只检查重新编译的 C 源文件
# C=2：检查所有 C 源文件（无论是否需要重新编译）
# sparse 是一个静态分析工具，能检测内核代码中的地址空间错误、
# 端序问题等。详见 Documentation/dev-tools/sparse.rst

ifeq ("$(origin C)", "command line")
  KBUILD_CHECKSRC = $(C)
endif
ifndef KBUILD_CHECKSRC
  KBUILD_CHECKSRC = 0
endif
export KBUILD_CHECKSRC

# ============================================================================
# Rust 代码的 Clippy linter
# ============================================================================
# 使用 'make CLIPPY=1' 启用
ifeq ("$(origin CLIPPY)", "command line")
  KBUILD_CLIPPY := $(CLIPPY)
endif
export KBUILD_CLIPPY

# ============================================================================
# 外部模块构建支持（M= 参数）
# ============================================================================
# 使用 make M=dir 或设置环境变量 KBUILD_EXTMOD 来指定外部模块目录
# 命令行指定的 M= 优先级高于环境变量
# MO= 用于指定外部模块的输出目录
ifeq ("$(origin M)", "command line")
  KBUILD_EXTMOD := $(M)
endif

ifeq ("$(origin MO)", "command line")
  KBUILD_EXTMOD_OUTPUT := $(MO)
endif

# 不支持同时构建多个外部模块
$(if $(word 2, $(KBUILD_EXTMOD)), \
	$(error building multiple external modules is not supported))

# 外部模块路径不能包含 '%' 或 ':'（这些是 Make 和路径中的特殊字符）
$(foreach x, % :, $(if $(findstring $x, $(KBUILD_EXTMOD)), \
	$(error module directory path cannot contain '$x')))

# 移除路径末尾的斜杠
ifneq ($(filter %/, $(KBUILD_EXTMOD)),)
KBUILD_EXTMOD := $(shell dirname $(KBUILD_EXTMOD).)
endif

export KBUILD_EXTMOD

# ============================================================================
# 额外警告开关（W= 参数）
# ============================================================================
ifeq ("$(origin W)", "command line")
  KBUILD_EXTRA_WARN := $(W)
endif
export KBUILD_EXTRA_WARN

# ============================================================================
# 输出目录设置（O= 参数）
# ============================================================================
# Kbuild 默认在当前目录保存输出文件，不必与源码目录相同。
# 两种指定输出目录的方式：
#   1) 命令行：make O=dir/to/store/output/files/
#   2) 环境变量：export KBUILD_OUTPUT=dir/to/store/output/files/
#   O= 的优先级高于 KBUILD_OUTPUT 环境变量
#
# 使用独立输出目录的好处：
#   - 一份源码可同时为多个架构/配置构建
#   - 源码目录保持干净

ifeq ("$(origin O)", "command line")
  KBUILD_OUTPUT := $(O)
endif

# 确定 objtree（内核目标树）和 output（工作输出目录）
ifdef KBUILD_EXTMOD
    ifdef KBUILD_OUTPUT
        objtree := $(realpath $(KBUILD_OUTPUT))
        $(if $(objtree),,$(error specified kernel directory "$(KBUILD_OUTPUT)" does not exist))
    else
        objtree := $(abs_srctree)
    endif
    # 外部模块的输出位置：
    # 如果 Make 在内核目录（源码或构建目录）中调用，模块在 $(KBUILD_EXTMOD) 中构建
    # 否则在当前目录构建（向后兼容）
    output := $(or $(KBUILD_EXTMOD_OUTPUT),$(if $(filter $(CURDIR),$(objtree) $(abs_srctree)),$(KBUILD_EXTMOD)))
    # KBUILD_EXTMOD 可能是相对路径，在 Make 改变工作目录之前记录其绝对路径
    srcroot := $(realpath $(KBUILD_EXTMOD))
    $(if $(srcroot),,$(error specified external module directory "$(KBUILD_EXTMOD)" does not exist))
else
    objtree := .
    output := $(KBUILD_OUTPUT)
endif

export objtree srcroot

# 如果需要改变工作目录，先创建输出目录
ifneq ($(output),)
# $(realpath ...) 在路径不存在时返回空，所以先 mkdir -p
$(shell mkdir -p "$(output)")
# $(realpath ...) 解析符号链接
abs_output := $(realpath $(output))
$(if $(abs_output),,$(error failed to create output directory "$(output)"))
endif

# 源码路径不能包含空格或冒号
ifneq ($(words $(subst :, ,$(abs_srctree))), 1)
$(error source directory cannot contain spaces or colons)
endif

# 标记第一遍 make 已完成，后续递归调用将进入第二遍
export sub_make_done := 1

endif # sub_make_done

# ============================================================================
# 子 make 调用逻辑（need-sub-make）
# ============================================================================
# 内核构建可能需要在输出目录中进行（与源码目录分离）。
# 为了正确处理这种情况，顶层 Makefile 设计了"两次调用"机制：
#
# 第一次调用（用户在源码目录或任意目录运行 make）：
#   1. 读取 Makefile，设置所有变量
#   2. 检测是否需要切换到不同的工作目录
#   3. 检测 MAKEFLAGS 是否需要设置 --no-print-directory
#   4. 如果上述任一条件满足，设置 need-sub-make := 1
#
# 第二次调用（由 __sub-make 触发）：
#   1. 在正确的输出目录中运行
#   2. 携带正确的 MAKEFLAGS（包含 --no-print-directory）
#   3. 此时 sub_make_done=1，跳过第一遍的设置，直接进入第二遍
#
# 需要子 make 的两种情况：
#   1. abs_output != CURDIR：输出目录与当前目录不同
#      - 用户使用了 O= 指定输出目录，或构建外部模块
#      - 需要 cd 到输出目录后再运行 make
#   2. --no-print-directory 不在 MAKEFLAGS 中：
#      - GNU Make 4.4.1 改变了行为：即使在最终目录中也不会自动添加此选项
#      - 显式添加 --no-print-directory 可以抑制 "Entering/Leaving directory" 消息
#      - 这是为了在并行构建（make -j）时保持输出整洁

ifeq ($(abs_output),$(CURDIR))
# 已在最终工作目录，抑制 "Entering directory ..." 消息
no-print-directory := --no-print-directory
else
# 输出目录与当前目录不同，需要递归切换到输出目录
need-sub-make := 1
endif

ifeq ($(filter --no-print-directory, $(MAKEFLAGS)),)
# 如果 --no-print-directory 未设置，再递归一次来设置它
need-sub-make := 1
endif

ifeq ($(need-sub-make),1)

PHONY += $(MAKECMDGOALS) __sub-make

$(filter-out $(this-makefile), $(MAKECMDGOALS)) __all: __sub-make
	@:

# 在输出目录中第二次调用 make，传递相关变量
# -C $(abs_output)：切换到输出目录
# -f $(abs_srctree)/Makefile：使用源码树中的 Makefile
__sub-make:
	$(Q)$(MAKE) $(no-print-directory) -C $(abs_output) \
	-f $(abs_srctree)/Makefile $(MAKECMDGOALS)

else # need-sub-make

# ============================================================================
# 第二遍 make（最终调用）：以下是真正的构建逻辑
# ============================================================================

# 对于外部模块，srcroot 已在上面设置；否则指向源码树
ifndef KBUILD_EXTMOD
srcroot := $(abs_srctree)
endif

# 判断是否为源码树外构建
ifeq ($(srcroot),$(CURDIR))
building_out_of_srctree :=
else
export building_out_of_srctree := 1
endif

# 将 srcroot 转换为相对路径（如果可能），便于编译器输出更短的文件路径
ifdef KBUILD_ABS_SRCTREE
    # 强制使用绝对路径，不做转换
else ifeq ($(srcroot),$(CURDIR))
    # 在源码目录中构建
    srcroot := .
else ifeq ($(srcroot)/,$(dir $(CURDIR)))
    # 在源码目录的子目录中构建（例如 make O=build）
    srcroot := ..
endif

# srctree：最终使用的源码树路径
export srctree := $(if $(KBUILD_EXTMOD),$(abs_srctree),$(srcroot))

# VPATH：告诉 make 在源码树中查找依赖文件
ifdef building_out_of_srctree
export VPATH := $(srcroot)
else
VPATH :=
endif

# ============================================================================
# 目标分类：区分配置目标、构建目标、清理目标
# ============================================================================
# 内核 Makefile 需要知道用户调用的目标是否需要 .config 配置文件，
# 以及是否需要运行 syncconfig 来同步配置。为此定义了三类目标：
#
#   no-dot-config-targets：不需要 .config 文件的目标
#     - 清理目标（clean, mrproper）、帮助目标（help）、标签生成（tags, cscope）
#     - 版本查询（kernelversion）、打包（%src-pkg）等
#     - 这些目标可以直接运行，无需任何配置
#
#   no-sync-config-targets：不需要同步配置的目标
#     - 包含所有 no-dot-config-targets，加上安装和发布目标
#     - 这些目标需要 .config 存在，但不需要它是最新的
#     - 例如 modules_install 需要知道 MODLIB 路径，但不关心配置是否变化
#
#   不在上述列表中的目标（如 vmlinux, modules）：
#     - 需要 .config 存在且是最新的
#     - 如果 .config 比 auto.conf 更新，会自动触发 syncconfig
#
# 特别注意：允许同时指定多个目标，包括混合 *config 目标和构建目标。
# 例如 'make oldconfig all'。如果检测到混合目标，会逐个顺序处理（__build_one_by_one）。

version_h := include/generated/uapi/linux/version.h

# 清理相关目标
clean-targets := %clean mrproper cleandocs

# 不需要 .config 文件的目标列表（不触发配置检查）
no-dot-config-targets := $(clean-targets) \
			 cscope gtags TAGS tags help% %docs check% coccicheck \
			 $(version_h) headers headers_% archheaders archscripts \
			 %asm-generic kernelversion %src-pkg dt_binding_check \
			 outputmakefile rustavailable rustfmt rustfmtcheck \
			 run-command

# 不需要同步配置的目标（除了上面的，还包括安装和发布相关目标）
no-sync-config-targets := $(no-dot-config-targets) %install modules_sign kernelrelease \
			  image_name

# 单一文件构建目标的后缀列表
single-targets := %.a %.i %.ko %.lds %.ll %.lst %.mod %.o %.rsi %.s %/

# 状态标志初始化
config-build	:=
# 是否仅为配置目标
mixed-build	:=
# 是否为混合目标（配置+构建）
need-config	:= 1
# 是否需要 .config
may-sync-config	:= 1
# 是否可以同步配置
single-build	:=
# 是否为单一文件构建

# 如果所有目标都在 no-dot-config-targets 列表中，不需要 .config
ifneq ($(filter $(no-dot-config-targets), $(MAKECMDGOALS)),)
    ifeq ($(filter-out $(no-dot-config-targets), $(MAKECMDGOALS)),)
        need-config :=
    endif
endif

# 如果所有目标都在 no-sync-config-targets 列表中，不需要同步配置
ifneq ($(filter $(no-sync-config-targets), $(MAKECMDGOALS)),)
    ifeq ($(filter-out $(no-sync-config-targets), $(MAKECMDGOALS)),)
        may-sync-config :=
    endif
endif

# 是否需要编译器检测
need-compiler := $(may-sync-config)

# 外部模块不需要同步配置
ifneq ($(KBUILD_EXTMOD),)
    may-sync-config :=
endif

# 检测是否为纯配置构建
ifeq ($(KBUILD_EXTMOD),)
    ifneq ($(filter %config,$(MAKECMDGOALS)),)
        config-build := 1
        ifneq ($(words $(MAKECMDGOALS)),1)
            mixed-build := 1
        endif
    endif
endif

# 单一目标和普通构建目标不能同时进行
ifneq ($(filter $(single-targets), $(MAKECMDGOALS)),)
    single-build := 1
    ifneq ($(filter-out $(single-targets), $(MAKECMDGOALS)),)
        mixed-build := 1
    endif
endif

# "make -j clean all" 或 "make -j mrproper defconfig all" 等混合目标检测
ifneq ($(filter $(clean-targets),$(MAKECMDGOALS)),)
    ifneq ($(filter-out $(clean-targets),$(MAKECMDGOALS)),)
        mixed-build := 1
    endif
endif

# install 和 modules_install 也需要逐个处理
ifneq ($(filter install,$(MAKECMDGOALS)),)
    ifneq ($(filter modules_install,$(MAKECMDGOALS)),)
        mixed-build := 1
    endif
endif

# ============================================================================
# 混合目标处理
# ============================================================================
# 当用户指定了混合目标（如配置+构建），逐个顺序执行每个目标
ifdef mixed-build

PHONY += $(MAKECMDGOALS) __build_one_by_one

$(MAKECMDGOALS): __build_one_by_one
	@:

__build_one_by_one:
	$(Q)set -e; \
	for i in $(MAKECMDGOALS); do \
		$(MAKE) -f $(srctree)/Makefile $$i; \
	done

else # !mixed-build

# ============================================================================
# Kbuild 核心函数库（scripts/Kbuild.include）
# ============================================================================
# scripts/Kbuild.include 是整个 Kbuild 构建系统的基础设施文件。
# 它定义了大量辅助变量、宏和函数，支撑了内核的高效增量编译。
# 以下是各函数的详细分类和使用说明。
#
# ============================================================================
# 一、基础辅助变量
# ============================================================================
# 这些变量将常用字符定义为 Make 变量，方便在函数中使用。
#
#   comma   := ,           # 逗号，用于 $(subst $(comma),...)
#   quote   := "           # 双引号
#   squote  := '           # 单引号
#   empty   :=             # 空字符串（用于条件判断中的占位）
#   space   := $(empty) $(empty)  # 单个空格（Make 会忽略变量值尾部的空格，
#                                      所以需要两个 $(empty) 来产生一个空格）
#   space_escape := _-_SPACE_-_   # 空格的转义表示，用于将命令行中的空格
#                                       替换为此标记，以便在 .cmd 文件中存储
#   pound   := \#          # 井号字符（Make 的注释符），用于生成 #define 等
#   newline                # 换行符，用 define/endef 定义，包含两个空行
#     （在多行文本替换场景中使用）
#
# ============================================================================
# 二、数值比较函数
# ============================================================================
# 用于比较两个数字（如编译器版本号、链接器版本号）。
#
#   $(call test-lt, $(VAL1), $(VAL2))
#     判断 VAL1 < VAL2。使用 Make >= 4.4 的内置 $(intcmp ...) 高效实现，
#     旧版本 Make 回退到 shell test 命令。
#     返回值：y（真）或空（假）
#
#   $(call test-le, $(VAL1), $(VAL2))
#     判断 VAL1 <= VAL2。通过反转参数调用 test-ge 实现。
#
#   $(call test-ge, $(VAL1), $(VAL2))
#     判断 VAL1 >= VAL2。
#
#   $(call test-gt, $(VAL1), $(VAL2))
#     判断 VAL1 > VAL2。
#
#   典型用法：
#     ifeq ($(call test-ge, $(CONFIG_LLD_VERSION), 150000),y)
#     KBUILD_LDFLAGS += --foo
#     endif
#
# ============================================================================
# 三、目标文件名处理函数
# ============================================================================
# 这些函数用于对 Make 的自动变量（$@ 等）进行变换，生成派生文件名。
#
#   $(dot-target)
#     获取以 '.' 为前缀的目标名。foo/bar.o => foo/.bar.o
#     主要用于生成 .*.cmd 文件的路径（存储构建命令和依赖）。
#
#   $(tmp-target)
#     获取以 '.tmp_' 为前缀的目标名。foo/bar.o => foo/.tmp_bar.o
#     用于生成临时文件（如 filechk 机制中的临时输出）。
#
#   $(depfile)
#     获取依赖文件 (.d) 的路径，并将逗号替换为下划线。
#     foo/bar.o => foo/.bar.o.d
#     GCC/Clang 使用 -MMD 生成的 .d 文件不能包含逗号，
#     所以如果路径中有逗号会被转义。
#
#   $(basetarget)
#     获取目标的基本文件名（去除目录和扩展名）。
#     foo/bar/baz.o => baz
#
#   $(real-prereqs)
#     获取真实的前置条件（过滤掉 .PHONY 目标）。
#     用法：$(filter-out $(PHONY), $^)
#
#   $(newer-prereqs)
#     获取比目标更新的前置条件（即导致重新编译的依赖）。
#     用法：$(filter-out $(PHONY), $?)
#     $? 是 Make 自动变量，表示所有比目标更新的前置条件。
#     这是 if_changed 检测"是否需要重新构建"的关键。
#
# ============================================================================
# 四、字符串处理函数
# ============================================================================
#
#   $(call escsq, $(STRING))
#     转义单引号以便在 shell 的 echo 语句中使用。
#     将 ' 替换为 '\''  （结束字符串 → 转义的单引号 → 开始新字符串）。
#
#   $(call stringify, $(VALUE))
#     将值包装为 C 语言字符串字面量。
#     例如 $(call stringify,foo) => '"foo"'
#     用于向 C 编译命令传递字符串（通过 -D 宏定义）。
#
#   $(call make-cmd, $(1))
#     将命令 cmd_$(1) 转义为适合存储到 .cmd 文件中的形式。
#     做三重转义：
#       (1) $$ → $$$$（保护 Make 变量引用）
#       (2) #  → $(pound)（防止在 .cmd 文件中被当作注释）
#       (3) '  → '\''（适合单引号包裹的 shell 字符串）
#
# ============================================================================
# 五、文件操作函数
# ============================================================================
#
#   $(call read-file, $(PATH))
#     读取文件内容，将换行符替换为空格后返回。
#     使用 Make >= 4.2 的内置 $(file < ...) 实现，
#     旧版本回退到 $(shell cat ...)。
#     典型用法：KERNELRELEASE = $(call read-file, include/config/kernel.release)
#
#   $(kbuild-file)
#     获取子目录中的构建文件路径（优先使用 Kbuild，回退到 Makefile）。
#     用法：$(or $(wildcard $(src)/Kbuild),$(src)/Makefile)
#
#   $(call filechk, <name>)
#     ★ 文件内容检查机制 ★
#     用于生成那些"内容很少变化，但变化时需要触发重新编译"的文件。
#     工作流程：
#       1. 调用 filechk_<name> 命令，将输出写入临时文件 .tmp_<target>
#       2. 比较临时文件与现有文件的内容（cmp -s）
#       3. 如果内容不同（或目标不存在），用临时文件替换目标文件
#       4. 如果内容相同，不更新目标文件（避免触发不必要的重新编译）
#     典型用法：
#       define filechk_version.h
#         echo \#define LINUX_VERSION_CODE $(shell expr ...)
#       endef
#       include/generated/version.h: FORCE
#         $(call filechk,version.h)
#     注意：需要事先定义对应的 filechk_<name> 变量或函数。
#     依赖 FORCE 前置条件（始终执行检测，但只有变化时才更新文件）。
#
# ============================================================================
# 六、构建与清理快捷宏
# ============================================================================
#
#   $(build)=<dir>
#     展开为：-f $(srctree)/scripts/Makefile.build obj=<dir>
#     这是递归构建的核心。实际使用时加上 $(MAKE)：
#       $(Q)$(MAKE) $(build)=fs/ext4
#     等效于：
#       make -f scripts/Makefile.build obj=fs/ext4
#     Makefile.build 会读取 fs/ext4/Kbuild（或 Makefile）中的 obj-y/obj-m 列表，
#     编译该目录中的所有目标文件，并生成 built-in.a 归档。
#
#   $(clean)=<dir>
#     展开为：-f $(srctree)/scripts/Makefile.clean obj=<dir>
#     用于清理指定目录：
#       $(Q)$(MAKE) $(clean)=fs/ext4
#     等效于：
#       make -f scripts/Makefile.clean obj=fs/ext4
#
# ============================================================================
# 七、日志输出系统
# ============================================================================
# Kbuild 有三种日志输出级别，通过 $(quiet) 变量控制：
#
#   $(quiet) == ""       详细模式（V=1）
#     - 打印完整的编译命令行
#     - 等于用户直接看到了 gcc/ld 的完整调用
#
#   $(quiet) == "quiet_" 简洁模式（默认）
#     - 只打印短标签（如 CC, LD, AR）+ 目标文件名
#     - 例如 "  CC      mm/page_alloc.o"
#
#   $(quiet) == "silent_" 静默模式（make -s）
#     - 完全不打印任何输出
#     - 即使编译错误也只打印 stderr
#
#   $(kecho)
#     条件输出（仅非静默模式下打印）。
#     展开为: $($(quiet)kecho)
#       silent_ 时 → :  (shell 的 no-op 命令)
#       quiet_  时 → echo
#       空      时 → echo
#
#   $(call escsq,$(quiet_cmd_$(1))$(why))
#     quiet_cmd_$(1) 是命令的简短标签，如 "CC" "LD" "AR"
#     $(why) 在 V=2 时追加重新编译的原因
#
#   自定义构建命令的模板：
#     quiet_cmd_myrule = MYRULE  $@       # 简洁输出
#            cmd_myrule = gcc -c -o $@ $<  # 实际命令
#     然后使用: $(call cmd,myrule)
#
# ============================================================================
# 八、命令执行函数 (cmd)
# ============================================================================
#
#   $(call cmd, <name>)
#     ★ 核心命令执行函数 ★
#     展开和执行预定义的 cmd_<name> 命令，支持：
#       - 根据 $(quiet) 自动选择日志级别
#       - 中断时自动删除不完整的目标文件（信号陷阱）
#       - 如果命令未定义，静默跳过（什么都不做）
#
#     执行流程：
#       1. 如果 cmd_$(1) 未定义 → 执行 : (no-op)
#       2. 设置 set -e（任何命令失败则退出）
#       3. 根据 $(quiet) 打印日志（无/简短/完整）
#       4. 设置信号陷阱（SIGHUP/INT/QUIT/TERM/PIPE → 删除 $@）
#       5. 执行 cmd_$(1)
#
#     典型用法：
#       quiet_cmd_link = LD      $@
#              cmd_link = $(LD) -o $@ $^
#       mytarget: mytarget.o
#         $(call cmd,link)
#
#   $(delete-on-interrupt)
#     信号中断保护：当 make 被 Ctrl-C 中断时，自动删除不完整的目标文件。
#     对 .PHONY 目标跳过（因为它们没有对应的文件）。
#     捕获的信号：SIGHUP, SIGINT, SIGQUIT, SIGTERM, SIGPIPE
#     注意：GNU Make 默认就会删除中断时未完成的目标，但通过管道重定向
#     stderr 时这个机制会失效（SIGPIPE 问题），所以需要显式设置陷阱。
#
# ============================================================================
# 九、依赖跟踪与增量构建（if_changed 系列）
# ============================================================================
# 这是 Kbuild 实现高效增量编译的核心机制。它比较"当前命令+依赖"
# 与"上次构建时的命令+依赖"，只有发生变化时才重新执行命令。
#
# ---- 检测条件 ----
#   $(if-changed-cond)
#     综合判断"是否需要重新构建"的条件：
#       (1) $(newer-prereqs)：有前置条件比目标更新
#       (2) $(cmd-check)：命令命令行发生了变化
#       (3) $(check-FORCE)：检查 FORCE 前置条件是否存在
#     以上任一条件非空即触发重新构建。
#
#   $(cmd-check)
#     比较当前命令行与上次保存的命令行（存储在 .<target>.cmd 文件中）。
#     如果相同则返回空（无需重建），不同则返回差异。
#     可以通过 KBUILD_NOCMDDEP=1 跳过此检查（只检查目标是否存在）。
#
#   $(check-FORCE)
#     检查 FORCE 是否在前置条件列表中。
#     如果目标没有 FORCE 前置条件，发出警告。
#     FORCE 是 .PHONY 目标，确保规则始终"有机会"被检查。
#
# ---- 三个变体 ----
#
#   $(call if_changed, <name>)
#     ★ 最常用的增量构建宏 ★
#     如果 if-changed-cond 非空（需要重建），则：
#       1. 执行 $(call cmd,<name>)
#       2. 将当前命令行保存到 .<target>.cmd 文件（通过 printf）
#     如果不需要重建：跳过（@: 即什么都不做）
#     典型用法：
#       foo.o: foo.c FORCE
#         $(call if_changed,foo_o)
#     需要事先定义：
#         quiet_cmd_foo_o = CC      $@
#                cmd_foo_o = $(CC) -c -o $@ $<
#
#   $(call if_changed_dep, <name>)
#     ★ 带依赖文件处理的增量构建宏 ★
#     与 if_changed 类似，但额外使用 fixdep 工具处理 .d 依赖文件。
#     fixdep 会：
#       1. 解析 gcc -MMD 生成的 .d 文件（列出头文件依赖）
#       2. 额外记录依赖的头文件中引用的 Kconfig 配置符号
#       3. 将所有依赖信息合并到 .<target>.cmd 文件中
#     这样当相关头文件或配置选项变化时，目标也会被重新编译。
#     典型用法（编译 C 文件，自动依赖跟踪）：
#       %.o: %.c FORCE
#         $(call if_changed_dep,cc_o_c)
#
#   $(call if_changed_rule, <name>)
#     ★ 执行自定义规则的增量构建宏 ★
#     与 if_changed 不同，它执行 rule_$(1) 而不是 cmd_$(1)。
#     用于需要复杂多行规则的场景（如链接 vmlinux）。
#     典型用法：
#       vmlinux: vmlinux.o
#         $(call if_changed_rule,vmlinux_link)
#     需要事先定义 rule_vmlinux_link（而非 cmd_vmlinux_link）。
#
# ---- .cmd 文件机制 ----
#   每个构建的目标文件都会生成一个对应的 .<target>.cmd 文件，
#   例如 mm/page_alloc.o 对应 mm/.page_alloc.o.cmd。
#   该文件包含：
#     - savedcmd_<target> := <escaped command line>
#     - 目标文件的完整依赖列表（通过 fixdep 增强）
#   下次 make 时，这些 .cmd 文件被 include，提供 savedcmd_* 变量
#   和依赖信息，使 if_changed 能够判断是否需要重新构建。
#
# ============================================================================
# 十、why 调试机制（V=2）
# ============================================================================
#   当 make V=2 时，每个构建命令后会显示重新构建的原因。
#   检测顺序和输出：
#     (1) 目标是 .PHONY      → "due to target is PHONY"
#     (2) 目标文件不存在     → "due to target missing"
#     (3) 前置条件更新       → "due to: file1.h file2.h"
#     (4) 命令行变化         → "due to command line change"
#     (5) .cmd 文件缺失      → "due to missing .cmd file"
#     (6) 目标不在 $(targets) → "due to xxx not in $(targets)"
#          （这通常是 Kbuild 文件中的 bug——目标未在 targets 中注册）
#
# ============================================================================
# 十一、编译器功能检测函数（scripts/Makefile.compiler）
# ============================================================================
#   这些函数在 scripts/Makefile.compiler 中定义，用于检测编译器能力。
#
#   $(call cc-option, <flag>)
#     检测 C 编译器是否支持某个编译选项。
#     如果支持，返回该选项；否则返回空。
#     典型用法：KBUILD_CFLAGS += $(call cc-option, -fno-stack-protector)
#
#   $(call cc-option-yn, <flag>)
#     与 cc-option 类似，但返回 'y' 或空（而非选项本身）。
#
#   $(call ld-option, <flag>)
#     检测链接器是否支持某个链接选项。
#
#   $(call rustc-option-yn, <flag>)
#     检测 Rust 编译器是否支持某个选项。
#
#   $(call rustc-min-version, <version>)
#     检查 Rust 编译器版本是否 >= 指定版本。
#
#   $(call as-option, <flag>)
#     检测汇编器是否支持某个选项。
# ============================================================================
include $(srctree)/scripts/Kbuild.include

# ============================================================================
# read-file 示例：读取 include/config/kernel.release 文件内容
# read-file 将文件中的换行符替换为空格，返回单行字符串。
# 如果文件不存在，返回空字符串。
# ============================================================================
KERNELRELEASE = $(call read-file, $(objtree)/include/config/kernel.release)
KERNELVERSION = $(VERSION)$(if $(PATCHLEVEL),.$(PATCHLEVEL)$(if $(SUBLEVEL),.$(SUBLEVEL)))$(EXTRAVERSION)
export VERSION PATCHLEVEL SUBLEVEL KERNELRELEASE KERNELVERSION

# 包含子架构检测脚本（确定 SUBARCH，如 x86_64, aarch64 等）
include $(srctree)/scripts/subarch.include

# ============================================================================
# 交叉编译设置
# ============================================================================
# ARCH：目标架构。可通过 make ARCH=arm64 指定，或使用环境变量。
#       默认为当前主机的架构。
# CROSS_COMPILE：交叉编译器前缀。
#       例如 make CROSS_COMPILE=aarch64-linux-gnu-
#       注意某些架构会在其 arch/*/Makefile 中设置 CROSS_COMPILE
ARCH		?= $(SUBARCH)

# UTS_MACHINE：用于 compile.h 中的架构名称
UTS_MACHINE 	:= $(ARCH)
# SRCARCH：源码树中 arch/ 下的实际目录名（可能与 ARCH 不同，如 i386 → x86）
SRCARCH 	:= $(ARCH)

# 架构映射：某些不同的 ARCH 名称共用同一套源码目录
# 例如 i386 和 x86_64 都使用 arch/x86/

# x86 架构映射
ifeq ($(ARCH),i386)
        SRCARCH := x86
endif
ifeq ($(ARCH),x86_64)
        SRCARCH := x86
endif

# sparc 架构映射
ifeq ($(ARCH),sparc32)
       SRCARCH := sparc
endif
ifeq ($(ARCH),sparc64)
       SRCARCH := sparc
endif

# parisc 架构映射
ifeq ($(ARCH),parisc64)
       SRCARCH := parisc
endif

# 判断是否为交叉编译（目标架构 != 主机架构）
export cross_compiling :=
ifneq ($(SRCARCH),$(SUBARCH))
cross_compiling := 1
endif

# 内核配置文件（默认 .config）
KCONFIG_CONFIG	?= .config
export KCONFIG_CONFIG

# kbuild 使用的 shell（固定为 sh）
CONFIG_SHELL := sh

# 大文件支持标志（用于 32 位系统上处理 >2GB 文件）
HOST_LFS_CFLAGS := $(shell getconf LFS_CFLAGS 2>/dev/null)
HOST_LFS_LDFLAGS := $(shell getconf LFS_LDFLAGS 2>/dev/null)
HOST_LFS_LIBS := $(shell getconf LFS_LIBS 2>/dev/null)

# ============================================================================
# 工具链选择：LLVM/Clang vs GCC
# ============================================================================
# LLVM=1：使用系统中默认的 LLVM 工具链
# LLVM=/path/to/llvm/：指定 LLVM 安装路径前缀
# LLVM=-version：指定 LLVM 版本后缀
ifneq ($(LLVM),)
ifneq ($(filter %/,$(LLVM)),)
LLVM_PREFIX := $(LLVM)
else ifneq ($(filter -%,$(LLVM)),)
LLVM_SUFFIX := $(LLVM)
else ifneq ($(LLVM),1)
$(error Invalid value for LLVM, see Documentation/kbuild/llvm.rst)
endif

HOSTCC	= $(LLVM_PREFIX)clang$(LLVM_SUFFIX)
HOSTCXX	= $(LLVM_PREFIX)clang++$(LLVM_SUFFIX)
else
HOSTCC	= gcc
HOSTCXX	= g++
endif
HOSTRUSTC = rustc
HOSTPKG_CONFIG	= pkg-config

# 内核文档生成工具
KERNELDOC       = $(srctree)/tools/docs/kernel-doc
export KERNELDOC

# ============================================================================
# 主机编译标志（用于编译在构建主机上运行的工具）
# ============================================================================
KBUILD_USERHOSTCFLAGS := -Wall -Wmissing-prototypes -Wstrict-prototypes \
			 -O2 -fomit-frame-pointer -std=gnu11
KBUILD_USERCFLAGS  := $(KBUILD_USERHOSTCFLAGS) $(USERCFLAGS)
KBUILD_USERLDFLAGS := $(USERLDFLAGS)

# Rust 通用编译标志（适用于内核和主机程序）
export rust_common_flags := --edition=2021 \
			    -Zbinary_dep_depinfo=y \
			    -Astable_features \
			    -Aunused_features \
			    -Dnon_ascii_idents \
			    -Dunsafe_op_in_unsafe_fn \
			    -Wmissing_docs \
			    -Wrust_2018_idioms \
			    -Wunreachable_pub \
			    -Wclippy::all \
			    -Wclippy::as_ptr_cast_mut \
			    -Wclippy::as_underscore \
			    -Wclippy::cast_lossless \
			    -Aclippy::collapsible_if \
			    -Aclippy::collapsible_match \
			    -Wclippy::ignored_unit_patterns \
			    -Aclippy::incompatible_msrv \
			    -Wclippy::mut_mut \
			    -Wclippy::needless_bitwise_bool \
			    -Aclippy::needless_lifetimes \
			    -Wclippy::no_mangle_with_rust_abi \
			    -Wclippy::ptr_as_ptr \
			    -Wclippy::ptr_cast_constness \
			    -Wclippy::ref_as_ptr \
			    -Wclippy::undocumented_unsafe_blocks \
			    -Aclippy::uninlined_format_args \
			    -Wclippy::unnecessary_safety_comment \
			    -Wclippy::unnecessary_safety_doc \
			    -Wrustdoc::missing_crate_level_docs \
			    -Wrustdoc::unescaped_backticks

# 主机编译器标志
KBUILD_HOSTCFLAGS   := $(KBUILD_USERHOSTCFLAGS) $(HOST_LFS_CFLAGS) \
		       $(HOSTCFLAGS) -I $(srctree)/scripts/include
KBUILD_HOSTCXXFLAGS := -Wall -O2 $(HOST_LFS_CFLAGS) $(HOSTCXXFLAGS) \
		       -I $(srctree)/scripts/include
KBUILD_HOSTRUSTFLAGS := $(rust_common_flags) -O -Cstrip=debuginfo \
			-Zallow-features=
KBUILD_HOSTLDFLAGS  := $(HOST_LFS_LDFLAGS) $(HOSTLDFLAGS)
KBUILD_HOSTLDLIBS   := $(HOST_LFS_LIBS) $(HOSTLDLIBS)
KBUILD_PROCMACROLDFLAGS := $(or $(PROCMACROLDFLAGS),$(KBUILD_HOSTLDFLAGS))

# ============================================================================
# 编译工具定义（CC, LD, AR, NM, OBJCOPY 等）
# ============================================================================
# 如果使用 LLVM=1，则使用 LLVM 工具链；否则使用 GNU 工具链
# 预处理器
CPP		= $(CC) -E
ifneq ($(LLVM),)
# C 编译器
CC		= $(LLVM_PREFIX)clang$(LLVM_SUFFIX)
# 链接器
LD		= $(LLVM_PREFIX)ld.lld$(LLVM_SUFFIX)
# 归档工具
AR		= $(LLVM_PREFIX)llvm-ar$(LLVM_SUFFIX)
# LLVM 位码链接器
LLVM_LINK	= $(LLVM_PREFIX)llvm-link$(LLVM_SUFFIX)
# 符号表工具
NM		= $(LLVM_PREFIX)llvm-nm$(LLVM_SUFFIX)
# 目标文件复制
OBJCOPY		= $(LLVM_PREFIX)llvm-objcopy$(LLVM_SUFFIX)
# 反汇编
OBJDUMP		= $(LLVM_PREFIX)llvm-objdump$(LLVM_SUFFIX)
# ELF 读取
READELF		= $(LLVM_PREFIX)llvm-readelf$(LLVM_SUFFIX)
# 符号剥离
STRIP		= $(LLVM_PREFIX)llvm-strip$(LLVM_SUFFIX)
else
CC		= $(CROSS_COMPILE)gcc
LD		= $(CROSS_COMPILE)ld
AR		= $(CROSS_COMPILE)ar
NM		= $(CROSS_COMPILE)nm
OBJCOPY		= $(CROSS_COMPILE)objcopy
OBJDUMP		= $(CROSS_COMPILE)objdump
READELF		= $(CROSS_COMPILE)readelf
STRIP		= $(CROSS_COMPILE)strip
endif
# 以下工具不随交叉编译改变
# Rust 编译器
RUSTC		= rustc
# Rust 文档生成器
RUSTDOC		= rustdoc
# Rust 格式化工具
RUSTFMT		= rustfmt
CLIPPY_DRIVER	= clippy-driver
# Rust linter
# Rust FFI 绑定生成器
BINDGEN		= bindgen
# BTF 调试信息生成工具
PAHOLE		= pahole
# BTF ID 解析器
RESOLVE_BTFIDS	= $(objtree)/tools/bpf/resolve_btfids/resolve_btfids
# 词法分析器生成器
LEX		= flex
# 语法分析器生成器
YACC		= bison
AWK		= awk
INSTALLKERNEL  := installkernel
PERL		= perl
PYTHON3		= python3
# 静态分析工具
CHECK		= sparse
BASH		= bash
# 内核压缩工具
KGZIP		= gzip
KBZIP2		= bzip2
KLZOP		= lzop
LZMA		= lzma
LZ4		= lz4
XZ		= xz
ZSTD		= zstd
TAR		= tar

# ============================================================================
# 编译器检查标志和通用标志
# ============================================================================
# sparse 检查器的标志
CHECKFLAGS     := -D__linux__ -Dlinux -D__STDC__ -Dunix -D__unix__ \
		  -Wbitwise -Wno-return-void -Wno-unknown-attribute $(CF)
# 不使用标准头文件目录的标志（后面会添加 -nostdinc）
NOSTDINC_FLAGS :=
# 模块/内核各自的编译、汇编、链接标志
CFLAGS_MODULE   =
RUSTFLAGS_MODULE =
AFLAGS_MODULE   =
LDFLAGS_MODULE  =
CFLAGS_KERNEL	=
RUSTFLAGS_KERNEL =
AFLAGS_KERNEL	=
LDFLAGS_vmlinux =

# ============================================================================
# 头文件包含路径
# ============================================================================
# USERINCLUDE：只引用 UAPI（用户空间 API）目录
# 用于需要引用用户空间可见头文件的情况
USERINCLUDE    := \
		-I$(srctree)/arch/$(SRCARCH)/include/uapi \
		-I$(objtree)/arch/$(SRCARCH)/include/generated/uapi \
		-I$(srctree)/include/uapi \
		-I$(objtree)/include/generated/uapi \
                -include $(srctree)/include/linux/compiler-version.h \
                -include $(srctree)/include/linux/kconfig.h

# LINUXINCLUDE：包含所有内核头文件目录（用于内核代码编译）
# 兼容 O= 选项（源码树外构建）
LINUXINCLUDE    := \
		-I$(srctree)/arch/$(SRCARCH)/include \
		-I$(objtree)/arch/$(SRCARCH)/include/generated \
		-I$(srctree)/include \
		-I$(objtree)/include \
		$(USERINCLUDE)

# ============================================================================
# 基础编译标志
# ============================================================================
# KBUILD_AFLAGS：汇编器标志
KBUILD_AFLAGS   := -D__ASSEMBLY__ -fno-PIE

# KBUILD_CFLAGS：C 编译器标志
KBUILD_CFLAGS :=
# 使用 short 型 wchar_t（与 ABI 兼容）
KBUILD_CFLAGS += -fshort-wchar
# char 默认为 unsigned（与 ARM 架构一致）
KBUILD_CFLAGS += -funsigned-char
# 禁止 common 符号（防止重复定义问题）
KBUILD_CFLAGS += -fno-common
# 禁止位置无关可执行文件（内核使用固定地址）
KBUILD_CFLAGS += -fno-PIE
# 禁止严格别名优化（内核代码广泛使用类型双关）
KBUILD_CFLAGS += -fno-strict-aliasing

# KBUILD_CPPFLAGS：预处理器标志，定义 __KERNEL__ 宏
KBUILD_CPPFLAGS := -D__KERNEL__

# KBUILD_RUSTFLAGS：Rust 编译器标志
KBUILD_RUSTFLAGS := $(rust_common_flags) \
		    -Cpanic=abort -Cembed-bitcode=n -Clto=n \
		    -Cforce-unwind-tables=n -Ccodegen-units=1 \
		    -Csymbol-mangling-version=v0 \
		    -Crelocation-model=static \
		    -Zfunction-sections=n \
		    -Wclippy::float_arithmetic

# 内核/模块特定的编译标志基础值
KBUILD_AFLAGS_KERNEL :=
KBUILD_CFLAGS_KERNEL :=
KBUILD_RUSTFLAGS_KERNEL :=
# 模块代码定义 MODULE 宏（用于条件编译）
KBUILD_AFLAGS_MODULE  := -DMODULE
KBUILD_CFLAGS_MODULE  := -DMODULE
KBUILD_RUSTFLAGS_MODULE := --cfg MODULE
KBUILD_LDFLAGS_MODULE :=
KBUILD_LDFLAGS :=
CLANG_FLAGS :=

# 如果启用 Clippy，使用 Clippy 代替 rustc 进行检查
ifeq ($(KBUILD_CLIPPY),1)
	RUSTC_OR_CLIPPY_QUIET := CLIPPY
	RUSTC_OR_CLIPPY = $(CLIPPY_DRIVER)
else
	RUSTC_OR_CLIPPY_QUIET := RUSTC
	RUSTC_OR_CLIPPY = $(RUSTC)
endif

# 允许在稳定版编译器中使用不稳定特性（Rust 内核开发需要）
export RUSTC_BOOTSTRAP := 1

# 在源码树外构建时也能找到 .clippy.toml 配置文件
export CLIPPY_CONF_DIR := $(srctree)

# ============================================================================
# 导出变量供子 make 和脚本使用
# ============================================================================
export ARCH SRCARCH CONFIG_SHELL BASH HOSTCC KBUILD_HOSTCFLAGS CROSS_COMPILE LD CC HOSTPKG_CONFIG
export RUSTC RUSTDOC RUSTFMT RUSTC_OR_CLIPPY_QUIET RUSTC_OR_CLIPPY BINDGEN LLVM_LINK
export HOSTRUSTC KBUILD_HOSTRUSTFLAGS
export CPP AR NM STRIP OBJCOPY OBJDUMP READELF PAHOLE RESOLVE_BTFIDS LEX YACC AWK INSTALLKERNEL
export PERL PYTHON3 CHECK CHECKFLAGS MAKE UTS_MACHINE HOSTCXX
export KGZIP KBZIP2 KLZOP LZMA LZ4 XZ ZSTD TAR
export KBUILD_HOSTCXXFLAGS KBUILD_HOSTLDFLAGS KBUILD_HOSTLDLIBS KBUILD_PROCMACROLDFLAGS LDFLAGS_MODULE
export KBUILD_USERCFLAGS KBUILD_USERLDFLAGS

export KBUILD_CPPFLAGS NOSTDINC_FLAGS LINUXINCLUDE OBJCOPYFLAGS KBUILD_LDFLAGS
export KBUILD_CFLAGS CFLAGS_KERNEL CFLAGS_MODULE
export KBUILD_RUSTFLAGS RUSTFLAGS_KERNEL RUSTFLAGS_MODULE
export KBUILD_AFLAGS AFLAGS_KERNEL AFLAGS_MODULE
export KBUILD_AFLAGS_MODULE KBUILD_CFLAGS_MODULE KBUILD_RUSTFLAGS_MODULE KBUILD_LDFLAGS_MODULE
export KBUILD_AFLAGS_KERNEL KBUILD_CFLAGS_KERNEL KBUILD_RUSTFLAGS_KERNEL

# find 命令中需要忽略的版本控制目录
export RCS_FIND_IGNORE := \( -name SCCS -o -name BitKeeper -o -name .svn -o    \
			  -name CVS -o -name .pc -o -name .hg -o -name .git \) \
			  -prune -o

# ============================================================================
# *config 目标和构建目标共享的规则
# ============================================================================

# ============================================================================
# $(build)= 示例：构建 scripts/basic/ 目录
# $(build)=scripts/basic 展开为：-f scripts/Makefile.build obj=scripts/basic
# 效果：递归调用 make 使用 Makefile.build 编译 scripts/basic/ 中的所有目标。
# fixdep 是 Kbuild 依赖跟踪的核心工具，用于处理 .d 文件并合并配置依赖。
# ============================================================================
PHONY += scripts_basic
scripts_basic: KBUILD_HOSTCFLAGS := $(KBUILD_HOSTCFLAGS)
scripts_basic: KBUILD_HOSTLDFLAGS := $(KBUILD_HOSTLDFLAGS)
scripts_basic:
	$(Q)$(MAKE) $(build)=scripts/basic

PHONY += outputmakefile
ifdef building_out_of_srctree
# ============================================================================
# outputmakefile：在输出目录中生成一个简单的 Makefile
# ============================================================================
# 在开始源码树外构建之前，确保源码树是干净的。
# 在输出目录中生成的 Makefile 允许用户在输出目录中直接运行 make。
# 同时生成 .gitignore 以忽略整个输出目录。

ifdef KBUILD_EXTMOD
print_env_for_makefile = \
	echo "export KBUILD_OUTPUT = $(objtree)"; \
	echo "export KBUILD_EXTMOD = $(realpath $(srcroot))" ; \
	echo "export KBUILD_EXTMOD_OUTPUT = $(CURDIR)"
else
print_env_for_makefile = \
	echo "export KBUILD_OUTPUT = $(CURDIR)"
endif

# 生成输出目录中的 Makefile（自动生成，导入环境变量后 include 主 Makefile）
filechk_makefile = { \
	echo "\# Automatically generated by $(abs_srctree)/Makefile: don't edit"; \
	$(print_env_for_makefile); \
	echo "include $(abs_srctree)/Makefile"; \
	}

$(objtree)/Makefile: FORCE
	$(call filechk,makefile)

# 防止源码树中的 Makefile 抑制此规则
PHONY += $(objtree)/Makefile

outputmakefile: $(objtree)/Makefile
ifeq ($(KBUILD_EXTMOD),)
	@if [ -f $(srctree)/.config -o \
		 -d $(srctree)/include/config -o \
		 -d $(srctree)/arch/$(SRCARCH)/include/generated ]; then \
		echo >&2 "***"; \
		echo >&2 "*** The source tree is not clean, please run 'make$(if $(findstring command line, $(origin ARCH)), ARCH=$(ARCH)) mrproper'"; \
		echo >&2 "*** in $(abs_srctree)";\
		echo >&2 "***"; \
		false; \
	fi
else
	@if [ -f $(srcroot)/modules.order ]; then \
		echo >&2 "***"; \
		echo >&2 "*** The external module source tree is not clean."; \
		echo >&2 "*** Please run 'make -C $(abs_srctree) M=$(realpath $(srcroot)) clean'"; \
		echo >&2 "***"; \
		false; \
	fi
endif
	$(Q)ln -fsn $(srcroot) source
	$(Q)test -e .gitignore || \
	{ echo "# this is build directory, ignore it"; echo "*"; } > .gitignore
endif

# ============================================================================
# 编译器版本检测
# ============================================================================
# CC_VERSION_TEXT 和 PAHOLE_VERSION 被 Kconfig 引用（用于检测编译器功能），
# 也被 include/config/auto.conf.cmd 用来检测版本变化触发重新配置。
# $(pound) 是一个 '#' 字符的变量，用于替换版本字符串中的 '#'。
CC_VERSION_TEXT = $(subst $(pound),,$(shell LC_ALL=C $(CC) --version 2>/dev/null | head -n 1))
RUSTC_VERSION_TEXT = $(subst $(pound),,$(shell $(RUSTC) --version 2>/dev/null))
PAHOLE_VERSION = $(shell $(srctree)/scripts/pahole-version.sh $(PAHOLE))

# 如果使用 clang，包含 clang 特定的 Makefile
ifneq ($(findstring clang,$(CC_VERSION_TEXT)),)
include $(srctree)/scripts/Makefile.clang
endif

# 配置目标也需要包含此文件，因为某些架构需要 cc-cross-prefix 来确定 CROSS_COMPILE
ifdef need-compiler
include $(srctree)/scripts/Makefile.compiler
endif

# ============================================================================
# 配置目标（*config targets）
# ============================================================================
ifdef config-build

# *config 目标处理：确保前置条件已更新，然后下降到 scripts/kconfig 中执行
# 包含架构特定的 Makefile 以设置 KBUILD_DEFCONFIG（默认配置）
include $(srctree)/arch/$(SRCARCH)/Makefile
export KBUILD_DEFCONFIG KBUILD_KCONFIG CC_VERSION_TEXT RUSTC_VERSION_TEXT PAHOLE_VERSION

config: outputmakefile scripts_basic FORCE
	$(Q)$(MAKE) $(build)=scripts/kconfig $@

%config: outputmakefile scripts_basic FORCE
	$(Q)$(MAKE) $(build)=scripts/kconfig $@

else #!config-build

# ============================================================================
# 构建目标（Build targets）
# ============================================================================
# 包括 vmlinux、架构特定目标、清理目标等。除了 *config 以外的所有目标。

# 对于外部模块，__all 依赖于 modules 而非 all
PHONY += all
ifeq ($(KBUILD_EXTMOD),)
__all: all
else
__all: modules
endif

targets :=

# ============================================================================
# 构建模式决策：内置（built-in）、模块（modular）或两者
# ============================================================================
KBUILD_MODULES :=
KBUILD_BUILTIN := y

# 如果只执行 "make modules"，不编译内置对象
ifeq ($(MAKECMDGOALS),modules)
  KBUILD_BUILTIN :=
endif

# "make <whatever> modules" — 在构建其他目标的同时也编译模块
# "make" 或 "make all" 也构建模块
ifneq ($(filter all modules nsdeps compile_commands.json clang-%,$(MAKECMDGOALS)),)
  KBUILD_MODULES := y
endif

ifeq ($(MAKECMDGOALS),)
  KBUILD_MODULES := y
endif

export KBUILD_MODULES KBUILD_BUILTIN

# ============================================================================
# 包含 .config 生成的配置头文件
# ============================================================================
ifdef need-config
include $(objtree)/include/config/auto.conf
endif

# C 语言方言选择：使用 GNU11 标准
CC_FLAGS_DIALECT := -std=gnu11
# 允许在结构体/联合体中匿名包含带标签的结构体或联合体（MS 扩展）
CC_FLAGS_DIALECT += $(CONFIG_CC_MS_EXTENSIONS)
# Clang 默认会对 GNU 和 MS 扩展发出警告，这里关闭它们
ifdef CONFIG_CC_IS_CLANG
CC_FLAGS_DIALECT += -Wno-gnu
CC_FLAGS_DIALECT += -Wno-microsoft-anon-tag
endif
export CC_FLAGS_DIALECT
KBUILD_CFLAGS += $(CC_FLAGS_DIALECT)

# ============================================================================
# 内核构建的核心组件
# ============================================================================
ifeq ($(KBUILD_EXTMOD),)
# 要链接进 vmlinux 的目标文件和要访问的子目录
core-y		:=
# 核心内核组件
drivers-y	:=
# 驱动组件
libs-y		:= lib/
# 库组件（默认包含 lib/）
endif # KBUILD_EXTMOD

# all: 目标是命令行上没有指定目标时的默认目标
# 默认构建 vmlinux，但架构 Makefile 通常会添加更多目标
all: vmlinux

# ============================================================================
# 代码覆盖率标志（GCOV）
# ============================================================================
CFLAGS_GCOV	:= -fprofile-arcs -ftest-coverage
ifdef CONFIG_CC_IS_GCC
# GCC 特定优化关闭
CFLAGS_GCOV	+= -fno-tree-loop-im
endif
export CFLAGS_GCOV

# ============================================================================
# 函数追踪器标志（FTRACE）
# ============================================================================
ifdef CONFIG_FUNCTION_TRACER
  # -pg 在函数入口生成 mcount 调用
  CC_FLAGS_FTRACE := -pg
endif

# ============================================================================
# 未使用跟踪点警告
# ============================================================================
# 使用 make UT=1 来警告定义了但从未调用的跟踪点
# 每个未使用的跟踪点在运行内核中最多占用 5KB 内存
ifdef CONFIG_TRACEPOINTS
ifeq ("$(origin UT)", "command line")
  WARN_ON_UNUSED_TRACEPOINTS := $(UT)
endif
endif # CONFIG_TRACEPOINTS
export WARN_ON_UNUSED_TRACEPOINTS

# ============================================================================
# 按 Rust 编译器版本的标志
# ============================================================================
# 这些标志类似于 rust_common_flags，但可能依赖 Rust 编译器版本。
# -Aclippy::precedence：Rust 1.85.0 扩展了此 lint，但 1.86.0 将其拆分为新的
# precedence_bits lint，所以仅在 1.86.0 之前抑制。
rust_common_flags_per_version := \
    $(if $(call rustc-min-version,108600),,-Aclippy::precedence)

rust_common_flags += $(rust_common_flags_per_version)
KBUILD_HOSTRUSTFLAGS += $(rust_common_flags_per_version) $(HOSTRUSTFLAGS)
KBUILD_RUSTFLAGS += $(rust_common_flags_per_version)

# ============================================================================
# 包含架构特定的 Makefile
# ============================================================================
# 架构 Makefile 定义架构特定的编译标志、要构建的目标等
include $(srctree)/arch/$(SRCARCH)/Makefile

# ============================================================================
# 配置同步（syncconfig）机制
# ============================================================================
# Kbuild 的配置系统分为两层：
#   1. 用户编辑层：.config（纯文本，key=value 格式）
#   2. 构建使用层：include/config/auto.conf（Makefile 可 include 的格式）
#                   include/generated/autoconf.h（C 头文件，#define 格式）
#                   include/generated/rustc_cfg（Rust cfg 标志）
#
# syncconfig 的作用：
#   当 .config 发生变化（用户修改了配置），syncconfig 读取 .config，
#   重新生成 auto.conf、autoconf.h、rustc_cfg 等文件，确保构建系统
#   使用的配置与用户设置一致。
#
# 配置同步的触发条件（多目标模式规则）：
#   如果 .config 的时间戳比 auto.conf 新（用户修改了配置），
#   或者 .config 的依赖（Kconfig* 文件）发生了变化，
#   则运行 syncconfig 重新同步所有生成的配置文件。
#   使用"多目标模式规则"技巧，一次 syncconfig 调用生成所有四个目标文件。
#
# auto.conf.cmd 的作用：
#   这个文件记录了 make 如何生成 auto.conf 的命令行和依赖。
#   它包含了所有 Kconfig* 文件的列表，如果任何 Kconfig 文件变化，
#   也会触发重新配置。
ifdef need-config
ifdef may-sync-config
# 读取 Kconfig* 文件的依赖关系，如果检测到配置变化则运行 syncconfig
include include/config/auto.conf.cmd

# 如果 .config 文件不存在，提示用户先创建配置
$(KCONFIG_CONFIG):
	@echo >&2 '***'
	@echo >&2 '*** Configuration file "$@" not found!'
	@echo >&2 '***'
	@echo >&2 '*** Please run some configurator (e.g. "make oldconfig" or'
	@echo >&2 '*** "make menuconfig" or "make xconfig").'
	@echo >&2 '***'
	@/bin/false

# ============================================================================
# 配置同步规则
# ============================================================================
# 构建过程中实际使用的配置文件存储在 include/generated/ 和 include/config/。
# 如果 .config 比 include/config/auto.conf 更新，则更新它们。
# 这里利用"多目标模式规则"技巧，syncconfig 只执行一次来生成所有目标。
# （升级到 GNU Make 4.3 后可使用分组目标 '&:'）
#
# 这里不使用 $(call cmd,...)，因为那会抑制 syncconfig 的提示，
# 导致用户看不到 Kconfig 在等待输入。
%/config/auto.conf %/config/auto.conf.cmd %/generated/autoconf.h %/generated/rustc_cfg: $(KCONFIG_CONFIG)
	$(Q)$(kecho) "  SYNC    $@"
	$(Q)$(MAKE) -f $(srctree)/Makefile syncconfig

else # !may-sync-config

# ============================================================================
# 外部模块和某些安装目标
# ============================================================================
# 需要 include/generated/autoconf.h 和 include/config/auto.conf 存在，
# 但不关心它们是否是最新的。如果文件缺失，显示错误消息。
checked-configs := $(addprefix $(objtree)/, include/generated/autoconf.h include/generated/rustc_cfg include/config/auto.conf)
missing-configs := $(filter-out $(wildcard $(checked-configs)), $(checked-configs))

ifdef missing-configs
PHONY += $(objtree)/include/config/auto.conf

$(objtree)/include/config/auto.conf:
	@echo   >&2 '***'
	@echo   >&2 '***  ERROR: Kernel configuration is invalid. The following files are missing:'
	@printf >&2 '***    - %s\n' $(missing-configs)
	@echo   >&2 '***  Run "make oldconfig && make prepare" on kernel source to fix it.'
	@echo   >&2 '***'
	@/bin/false
endif

endif # may-sync-config
endif # need-config

# ============================================================================
# 编译器优化和代码生成标志
# ============================================================================

# 禁止编译器删除空指针检查（内核有时有意依赖空指针解引用）
KBUILD_CFLAGS	+= -fno-delete-null-pointer-checks

# 优化级别选择
ifdef CONFIG_CC_OPTIMIZE_FOR_PERFORMANCE
KBUILD_CFLAGS += -O2
KBUILD_RUSTFLAGS += -Copt-level=2
else ifdef CONFIG_CC_OPTIMIZE_FOR_SIZE
KBUILD_CFLAGS += -Os
KBUILD_RUSTFLAGS += -Copt-level=s
endif

# Rust 调试断言和溢出检查（根据内核配置启用/禁用）
KBUILD_RUSTFLAGS += -Cdebug-assertions=$(if $(CONFIG_RUST_DEBUG_ASSERTIONS),y,n)
KBUILD_RUSTFLAGS += -Coverflow-checks=$(if $(CONFIG_RUST_OVERFLOW_CHECKS),y,n)

# ============================================================================
# 禁止条件加载的非条件化优化（GCC）
# ============================================================================
# 告诉 GCC 永远不要将条件加载替换为非条件加载
# 内核在某些情况下依赖条件加载的语义（例如 /dev/mem 的边界检查）
ifdef CONFIG_CC_IS_GCC
# gcc-10 将 --param=allow-store-data-races=0 重命名为 -fno-allow-store-data-races
KBUILD_CFLAGS	+= $(call cc-option,--param=allow-store-data-races=0)
KBUILD_CFLAGS	+= $(call cc-option,-fno-allow-store-data-races)
endif

# ============================================================================
# 可读汇编输出（用于调试）
# ============================================================================
ifdef CONFIG_READABLE_ASM
# 禁用使汇编列表难以阅读的优化：
#   -freorder-blocks：重新排序基本块
#   -fipa-cp-clone：创建专门的克隆函数
#   -fpartial-inlining：部分内联
KBUILD_CFLAGS += -fno-reorder-blocks -fno-ipa-cp-clone -fno-partial-inlining
endif

# ============================================================================
# 栈保护（Stack Protector）
# ============================================================================
stackp-flags-y                                    := -fno-stack-protector
stackp-flags-$(CONFIG_STACKPROTECTOR)             := -fstack-protector
stackp-flags-$(CONFIG_STACKPROTECTOR_STRONG)      := -fstack-protector-strong

KBUILD_CFLAGS += $(stackp-flags-y)

# ============================================================================
# 帧指针（Frame Pointer）
# ============================================================================
ifdef CONFIG_FRAME_POINTER
# 保留帧指针（用于栈回溯和调试）
KBUILD_CFLAGS	+= -fno-omit-frame-pointer -fno-optimize-sibling-calls
KBUILD_RUSTFLAGS += -Cforce-frame-pointers=y
else
# 某些架构（如 ARM Thumb2）不能用帧指针构建。
# FUNCTION_TRACER 添加 -pg，与 -fomit-frame-pointer 不兼容。
ifndef CONFIG_FUNCTION_TRACER
KBUILD_CFLAGS	+= -fomit-frame-pointer
endif
endif

# ============================================================================
# 栈变量初始化（安全加固）
# ============================================================================
# 使用 0xAA 模式初始化所有栈变量（检测未初始化的栈使用）
ifdef CONFIG_INIT_STACK_ALL_PATTERN
KBUILD_CFLAGS	+= -ftrivial-auto-var-init=pattern
endif

# 使用零值初始化所有栈变量
ifdef CONFIG_INIT_STACK_ALL_ZERO
KBUILD_CFLAGS	+= -ftrivial-auto-var-init=zero
ifdef CONFIG_CC_HAS_AUTO_VAR_INIT_ZERO_ENABLER
# Clang 的自动变量零初始化需要此额外标志（未来可能被移除）
CC_AUTO_VAR_INIT_ZERO_ENABLER := -enable-trivial-auto-var-init-zero-knowing-it-will-be-removed-from-clang
export CC_AUTO_VAR_INIT_ZERO_ENABLER
KBUILD_CFLAGS	+= $(CC_AUTO_VAR_INIT_ZERO_ENABLER)
endif
endif

# Clang counted_by 属性支持
ifdef CONFIG_CC_IS_CLANG
ifdef CONFIG_CC_HAS_COUNTED_BY_PTR
KBUILD_CFLAGS	+= -fexperimental-late-parse-attributes
endif
endif

# ============================================================================
# cc-option 示例：安全加固选项
# $(call cc-option,<flag>) 检测编译器是否支持该选项。
# 如果支持，返回该选项本身（如 -fzero-init-padding-bits=all）；
# 如果不支持，返回空（静默跳过）。
# 这样内核可以安全地使用新编译器的功能，同时兼容旧编译器。
# ============================================================================
# 在变量初始化时显式清零填充位（安全加固）
KBUILD_CFLAGS += $(call cc-option,-fzero-init-padding-bits=all)

# 虽然 VLA 已被移除，但 GCC 仍为 randomize_kstack_offset 生成不可达的栈探测。
# 对所有编译器禁用它。
KBUILD_CFLAGS	+= $(call cc-option, -fno-stack-clash-protection)

# 获取 GCC 值追踪产生的警告详情
KBUILD_CFLAGS	+= $(call cc-option, -fdiagnostics-show-context=2)

# 显示 __attribute__((warning/error)) 调用链的内联记录
KBUILD_CFLAGS	+= $(call cc-option, -fdiagnostics-show-inlining-chain)

# ============================================================================
# 函数退出时清零使用的寄存器（安全加固）
# ============================================================================
# 减少数据生命周期和 ROP 攻击面
ifdef CONFIG_ZERO_CALL_USED_REGS
KBUILD_CFLAGS	+= -fzero-call-used-regs=used-gpr
endif

# ============================================================================
# 函数追踪（Ftrace）详细配置
# ============================================================================
ifdef CONFIG_FUNCTION_TRACER
ifdef CONFIG_FTRACE_MCOUNT_USE_CC
  # 编译器在目标文件中记录 mcount 调用位置
  CC_FLAGS_FTRACE	+= -mrecord-mcount
  ifdef CONFIG_HAVE_NOP_MCOUNT
    ifeq ($(call cc-option-yn, -mnop-mcount),y)
      # 使用 nop 代替 mcount 调用（运行时动态替换）
      CC_FLAGS_FTRACE	+= -mnop-mcount
      CC_FLAGS_USING	+= -DCC_USING_NOP_MCOUNT
    endif
  endif
endif
ifdef CONFIG_FTRACE_MCOUNT_USE_OBJTOOL
  ifdef CONFIG_HAVE_OBJTOOL_NOP_MCOUNT
    CC_FLAGS_USING	+= -DCC_USING_NOP_MCOUNT
  endif
endif
ifdef CONFIG_FTRACE_MCOUNT_USE_RECORDMCOUNT
  ifdef CONFIG_HAVE_C_RECORDMCOUNT
    BUILD_C_RECORDMCOUNT := y
    export BUILD_C_RECORDMCOUNT
  endif
endif
# fentry（比 mcount 更高效的函数入口追踪）
ifdef CONFIG_HAVE_FENTRY
  ifeq ($(call cc-option-yn, -mfentry),y)
    CC_FLAGS_FTRACE	+= -mfentry
    CC_FLAGS_USING	+= -DCC_USING_FENTRY
  endif
endif
export CC_FLAGS_FTRACE
KBUILD_CFLAGS	+= $(CC_FLAGS_FTRACE) $(CC_FLAGS_USING)
KBUILD_AFLAGS	+= $(CC_FLAGS_USING)
endif

# ============================================================================
# 调试节区不匹配
# ============================================================================
ifdef CONFIG_DEBUG_SECTION_MISMATCH
KBUILD_CFLAGS += -fno-inline-functions-called-once
endif

# ============================================================================
# 死代码和数据消除（GC sections）
# ============================================================================
# 链接时移除未使用的函数和数据段，减小内核大小
ifdef CONFIG_LD_DEAD_CODE_DATA_ELIMINATION
KBUILD_CFLAGS_KERNEL += -ffunction-sections -fdata-sections
KBUILD_RUSTFLAGS_KERNEL += -Zfunction-sections=y
LDFLAGS_vmlinux += --gc-sections
endif

# ============================================================================
# 影子调用栈（Shadow Call Stack）
# ============================================================================
# 使用独立的影子栈来保护返回地址（ARM64 安全特性）
ifdef CONFIG_SHADOW_CALL_STACK
ifndef CONFIG_DYNAMIC_SCS
CC_FLAGS_SCS	:= -fsanitize=shadow-call-stack
KBUILD_CFLAGS	+= $(CC_FLAGS_SCS)
KBUILD_RUSTFLAGS += -Zsanitizer=shadow-call-stack
endif
export CC_FLAGS_SCS
endif

# ============================================================================
# 链接时优化（LTO — Link Time Optimization）
# ============================================================================
# LTO 允许编译器在链接时对整个程序进行跨翻译单元的优化，而不是仅在
# 编译单个 .c 文件时优化。这可以带来显著的性能和体积改善。
#
# 两种 LTO 模式（仅 Clang 支持）：
#
#   Full LTO（-flto）：
#     - 编译器将完整的 LLVM IR 位码嵌入目标文件
#     - 链接时对整个程序进行全局优化
#     - 优点：最大程度的优化（内联、死代码消除等跨文件生效）
#     - 缺点：链接时内存消耗巨大（整个内核的 IR 需要同时加载）
#
#   ThinLTO（-flto=thin）：
#     - 编译器只嵌入模块的摘要信息而非完整 IR
#     - 链接时并行进行优化（每个模块独立优化，然后合并）
#     - 优点：内存消耗可控，可以利用多核并行加速链接
#     - 缺点：优化效果略差于 Full LTO（但差距很小）
#     - 配合 -fsplit-lto-unit 将 IR 分割为更小的单元
#
# -fvisibility=hidden：
#   LTO 模式下将默认符号可见性设为 hidden，防止未导出的符号
#   泄漏到最终二进制中，同时帮助编译器进行更激进的优化。
#
# -mllvm -import-instr-limit=5：
#   限制 ThinLTO 的跨模块导入指令数量，在优化效果和代码膨胀之间取得平衡。
ifdef CONFIG_LTO_CLANG
ifdef CONFIG_LTO_CLANG_THIN
# ThinLTO：将模块汇总而非完整 IR 嵌入目标文件，链接时并行优化
CC_FLAGS_LTO	:= -flto=thin -fsplit-lto-unit
KBUILD_LDFLAGS += $(call ld-option,--lto-whole-program-visibility -mllvm -always-rename-promoted-locals=false)
else
# 完整 LTO
CC_FLAGS_LTO	:= -flto
endif
CC_FLAGS_LTO	+= -fvisibility=hidden

# 限制跨翻译单元的内联以减小二进制大小
KBUILD_LDFLAGS += -mllvm -import-instr-limit=5
endif

ifdef CONFIG_LTO
KBUILD_CFLAGS	+= -fno-lto $(CC_FLAGS_LTO)
KBUILD_AFLAGS	+= -fno-lto
export CC_FLAGS_LTO
endif

# ============================================================================
# CFI（控制流完整性）
# ============================================================================
# kCFI：内核控制流完整性，防止通过函数指针劫持控制流
ifdef CONFIG_CFI
CC_FLAGS_CFI	:= -fsanitize=kcfi
ifdef CONFIG_CFI_ICALL_NORMALIZE_INTEGERS
	CC_FLAGS_CFI	+= -fsanitize-cfi-icall-experimental-normalize-integers
endif
ifdef CONFIG_FINEIBT_BHI
	CC_FLAGS_CFI	+= -fsanitize-kcfi-arity
endif
ifdef CONFIG_RUST
	RUSTC_FLAGS_CFI   := -Zsanitizer=kcfi -Zsanitizer-cfi-normalize-integers
	KBUILD_RUSTFLAGS += $(RUSTC_FLAGS_CFI)
	export RUSTC_FLAGS_CFI
endif
KBUILD_CFLAGS	+= $(CC_FLAGS_CFI)
export CC_FLAGS_CFI
endif

# ============================================================================
# 浮点运算标志
# ============================================================================
# 架构可以定义标志来添加/移除浮点支持
CC_FLAGS_FPU	+= -D_LINUX_FPU_COMPILATION_UNIT
export CC_FLAGS_FPU
export CC_FLAGS_NO_FPU

# ============================================================================
# 函数对齐
# ============================================================================
ifneq ($(CONFIG_FUNCTION_ALIGNMENT),0)
# 设置最小函数对齐。优先使用新的 GCC -fmin-function-alignment 选项，
# 如果不可用则回退到 -falign-functions。
ifdef CONFIG_CC_HAS_MIN_FUNCTION_ALIGNMENT
KBUILD_CFLAGS += -fmin-function-alignment=$(CONFIG_FUNCTION_ALIGNMENT)
else
KBUILD_CFLAGS += -falign-functions=$(CONFIG_FUNCTION_ALIGNMENT)
endif
endif

# 不包含标准头文件（内核是独立编译的）
# 放在 arch Makefile 之后，因为 arch Makefile 可能覆盖 CC
NOSTDINC_FLAGS += -nostdinc

# 强制使用 C99 灵活数组（flexible arrays）以确保 UBSAN_BOUNDS 和 FORTIFY_SOURCE 的正确覆盖
KBUILD_CFLAGS += $(call cc-option, -fstrict-flex-arrays=3)

# 禁止有符号数和指针的无效"不会回绕"优化
KBUILD_CFLAGS	+= -fno-strict-overflow

# 确保不启用 -fstack-check（某些发行版如 gentoo 默认启用）
KBUILD_CFLAGS  += -fno-stack-check

# 如果可用，保存栈空间（GCC 特定）
ifdef CONFIG_CC_IS_GCC
KBUILD_CFLAGS   += -fconserve-stack
endif

# 禁止编译器将某些循环转换为 wcslen() 调用（内核没有标准 C 库）
KBUILD_CFLAGS += -fno-builtin-wcslen

# ============================================================================
# 将 __FILE__ 宏改为源码目录的相对路径
# ============================================================================
# 这对于源码树外构建很重要，使得编译输出中的路径更短且可重现
ifdef building_out_of_srctree
KBUILD_CPPFLAGS += -fmacro-prefix-map=$(srcroot)/=
ifeq ($(call rustc-option-yn, --remap-path-scope=macro),y)
KBUILD_RUSTFLAGS += --remap-path-prefix=$(srcroot)/= --remap-path-scope=macro
endif
endif

# ============================================================================
# 包含额外的 Makefile（警告、调试、消毒器、插件等）
# ============================================================================
include-y			:= scripts/Makefile.warn
include-$(CONFIG_DEBUG_INFO)	+= scripts/Makefile.debug
include-$(CONFIG_DEBUG_INFO_BTF)+= scripts/Makefile.btf
include-$(CONFIG_KASAN)		+= scripts/Makefile.kasan
# 内核地址消毒器
include-$(CONFIG_KCSAN)		+= scripts/Makefile.kcsan
# 内核并发消毒器
include-$(CONFIG_KMSAN)		+= scripts/Makefile.kmsan
# 内核内存消毒器
include-$(CONFIG_UBSAN)		+= scripts/Makefile.ubsan
# 未定义行为消毒器
include-$(CONFIG_KCOV)		+= scripts/Makefile.kcov
# 代码覆盖率
include-$(CONFIG_RANDSTRUCT)	+= scripts/Makefile.randstruct
# 结构体随机化
include-$(CONFIG_KSTACK_ERASE)	+= scripts/Makefile.kstack_erase
# 内核栈擦除
include-$(CONFIG_AUTOFDO_CLANG)	+= scripts/Makefile.autofdo
# 自动反馈导向优化
include-$(CONFIG_PROPELLER_CLANG)	+= scripts/Makefile.propeller
include-$(CONFIG_WARN_CONTEXT_ANALYSIS) += scripts/Makefile.context-analysis
include-$(CONFIG_GCC_PLUGINS)	+= scripts/Makefile.gcc-plugins
# GCC 插件支持

include $(addprefix $(srctree)/, $(include-y))

# scripts/Makefile.gcc-plugins 被有意放在最后 include。
# 不要在此行之后添加 $(call cc-option,...)，因为从干净的源码树构建时，
# GCC 插件此时还不存在。

# ============================================================================
# 用户提供的额外编译标志（最后添加，优先级最高）
# ============================================================================
# 允许用户通过 KAFLAGS, KCFLAGS 等环境变量覆盖编译标志
KBUILD_CPPFLAGS += $(KCPPFLAGS)
KBUILD_AFLAGS   += $(KAFLAGS)
KBUILD_CFLAGS   += $(KCFLAGS)
KBUILD_RUSTFLAGS += $(KRUSTFLAGS)

# ============================================================================
# 链接器标志
# ============================================================================
# --build-id=sha1：在 ELF 中嵌入 SHA1 构建 ID（用于调试和 kdump）
KBUILD_LDFLAGS_MODULE += --build-id=sha1
LDFLAGS_vmlinux += --build-id=sha1

# ============================================================================
# ld-option 示例：根据链接器版本条件添加标志
# $(call ld-option,<flag>) 检测链接器是否支持该选项，类似 cc-option。
# --pack-dyn-relocs=relr 用于生成紧凑的相对重定位（RELR），
# 可以减小重定位表的大小。如果链接器不支持则回退到 -z pack-relative-relocs。
# ============================================================================
# 标记栈为不可执行（NX 安全加固）
KBUILD_LDFLAGS	+= -z noexecstack
ifeq ($(CONFIG_LD_IS_BFD),y)
KBUILD_LDFLAGS	+= $(call ld-option,--no-warn-rwx-segments)
endif

# 剥离汇编符号（减小内核大小）
ifeq ($(CONFIG_STRIP_ASM_SYMS),y)
LDFLAGS_vmlinux	+= -X
endif

# RELR 相对重定位（减小重定位表大小）
ifeq ($(CONFIG_RELR),y)
LDFLAGS_vmlinux	+= $(call ld-option,--pack-dyn-relocs=relr,-z pack-relative-relocs)
endif

# 孤儿段警告：确保所有段都在链接脚本中显式命名
ifdef CONFIG_LD_ORPHAN_WARN
LDFLAGS_vmlinux += --orphan-handling=$(CONFIG_LD_ORPHAN_WARN_LEVEL)
endif

# 某些架构需要生成重定位条目
ifneq ($(CONFIG_ARCH_VMLINUX_NEEDS_RELOCS),)
LDFLAGS_vmlinux	+= --emit-relocs --discard-none
endif

# ============================================================================
# 用户空间程序编译标志对齐
# ============================================================================
# 将用户空间程序（如 tools/ 中的程序）的架构与内核对齐
USERFLAGS_FROM_KERNEL := --target=%

ifdef CONFIG_ARCH_USERFLAGS
KBUILD_USERCFLAGS += $(CONFIG_ARCH_USERFLAGS)
KBUILD_USERLDFLAGS += $(CONFIG_ARCH_USERFLAGS)
else
# 如果没有覆盖，同时继承位大小标志
USERFLAGS_FROM_KERNEL += -m32 -m64
endif

KBUILD_USERCFLAGS  += $(filter $(USERFLAGS_FROM_KERNEL), $(KBUILD_CPPFLAGS) $(KBUILD_CFLAGS))
KBUILD_USERLDFLAGS += $(filter $(USERFLAGS_FROM_KERNEL), $(KBUILD_CPPFLAGS) $(KBUILD_CFLAGS))

# 用户空间程序通过编译器间接链接，使用正确的链接器
ifdef CONFIG_CC_IS_CLANG
KBUILD_USERLDFLAGS += --ld-path=$(LD)
endif

# ============================================================================
# 静态检查器（sparse）配置
# ============================================================================
# 使用正确的架构运行检查器
CHECKFLAGS += --arch=$(ARCH)

# 确保检查器使用正确的端序
CHECKFLAGS += $(if $(CONFIG_CPU_BIG_ENDIAN),-mbig-endian,-mlittle-endian)

# 检查器需要正确的机器字长
CHECKFLAGS += $(if $(CONFIG_64BIT),-m64,-m32)

# 验证检查器是否可用且功能正常
ifneq ($(KBUILD_CHECKSRC), 0)
  ifneq ($(shell $(srctree)/scripts/checker-valid.sh $(CHECK) $(CHECKFLAGS)), 1)
    $(warning C=$(KBUILD_CHECKSRC) specified, but $(CHECK) is not available or not up to date)
    KBUILD_CHECKSRC = 0
  endif
endif

# ============================================================================
# 安装路径设置
# ============================================================================
# 内核安装涉及多个路径变量，支持交叉编译和根文件系统构建场景。
#
# KBUILD_IMAGE：默认构建的内核镜像名称
#   - 大多数架构为 vmlinux，某些架构可能不同（如 arm64 可能是 Image）
#   - 架构 Makefile 可以覆盖此值
export KBUILD_IMAGE ?= vmlinux

# INSTALL_PATH：内核镜像和 System.map 的安装位置
#   - 默认 /boot，可直接启动
#   - 交叉编译场景可设置为目标根文件系统的 /boot 目录
export	INSTALL_PATH ?= /boot

# INSTALL_DTBS_PATH：设备树 blob (.dtb) 的安装位置
#   - 默认在 /boot/dtbs/<kernel-release>/ 下
#   - 每个内核版本有独立的设备树目录，避免版本冲突
export INSTALL_DTBS_PATH ?= $(INSTALL_PATH)/dtbs/$(KERNELRELEASE)

# INSTALL_MOD_PATH：模块安装目录的前缀（用于交叉编译/构建根文件系统）
#   - 例如 make INSTALL_MOD_PATH=/mnt/target modules_install
#     会将模块安装到 /mnt/target/lib/modules/$(KERNELRELEASE)/
#   - 默认前缀为空（即直接安装到 /lib/modules/）
#
# MODLIB：模块的完整安装路径
#   - 公式：$(INSTALL_MOD_PATH)/lib/modules/$(KERNELRELEASE)
#   - 包含内核版本号，使多个内核版本的模块可以共存
#   - 例如：/lib/modules/7.1.0-rc3/kernel/drivers/net/
MODLIB	= $(INSTALL_MOD_PATH)/lib/modules/$(KERNELRELEASE)
export MODLIB

# ============================================================================
# prepare 阶段：在实际构建前准备必要文件
# ============================================================================
PHONY += prepare0

ifeq ($(KBUILD_EXTMOD),)

build-dir	:= .
# clean-dirs：清理时要处理的目录列表
clean-dirs	:= $(sort . Documentation \
		     $(patsubst %/,%,$(filter %/, $(core-) \
			$(drivers-) $(libs-))))

export ARCH_CORE	:= $(core-y)
export ARCH_LIB		:= $(filter %/, $(libs-y))
export ARCH_DRIVERS	:= $(drivers-y) $(drivers-m)

# ============================================================================
# vmlinux 链接相关的目标文件定义
# ============================================================================
# vmlinux 的构建分为三个步骤：
#   1. 归档：将所有子目录的 built-in.a 合并为 vmlinux.a
#   2. 部分链接：将 vmlinux.a 与 lib.a 链接为 vmlinux.o（单目标文件）
#   3. 最终链接：通过链接脚本将 vmlinux.o 链接为 vmlinux ELF 镜像
#
# KBUILD_VMLINUX_OBJS：要归档到 vmlinux.a 的内置目标文件
#   - built-in.a：顶层 built-in.a（包含 init/ 等核心目录）
#   - %/lib.a：每个以 / 结尾的 libs-y 条目的库归档
#   例如 libs-y := lib/ arch/arm64/lib/ 会生成 lib/lib.a 和 arch/arm64/lib/lib.a
#   这些都是由每个子目录中的 Makefile.build 创建的归档文件
#
# KBUILD_VMLINUX_LIBS：不需要归档、直接参与链接的库
#   - libs-y 中不以 / 结尾的条目（如某些外部库）
KBUILD_VMLINUX_OBJS := built-in.a $(patsubst %/, %/lib.a, $(filter %/, $(libs-y)))
KBUILD_VMLINUX_LIBS := $(filter-out %/, $(libs-y))

export KBUILD_VMLINUX_LIBS
# 链接脚本：定义 vmlinux 的内存布局（各段的排列顺序、对齐、起始地址等）
export KBUILD_LDS          := arch/$(SRCARCH)/kernel/vmlinux.lds

ifdef CONFIG_TRIM_UNUSED_KSYMS
# 为确定哪些导出符号真正被使用，需要也构建模块
# 然后根据模块实际引用的符号列表来裁剪未使用的导出符号
KBUILD_MODULES := y
endif

# ============================================================================
# vmlinux.a 的创建规则
# ============================================================================
# 将所有内置目标文件归档为一个 vmlinux.a
# AR 参数说明：
#   c — 创建归档（不警告）
#   D — 使用确定性模式（零 UID/GID/时间戳，确保可重现构建）
#   P — 使用完整路径名存储（支持同名文件在不同目录）
#   r — 替换已存在的成员
#   S — 不创建符号表索引（稍后手动创建）
#   T — 使用精简归档格式（Thin archive，仅存储引用而非复制数据）
#       这大大减少了中间归档的大小，因为目标文件数据不会被复制
# '$(AR) mPi' 需要 'T' 来绕过 llvm-ar <= 14 的 bug（关于精简归档中成员排序的问题）
quiet_cmd_ar_vmlinux.a = AR      $@
      cmd_ar_vmlinux.a = \
	rm -f $@; \
	$(AR) cDPrST $@ $(KBUILD_VMLINUX_OBJS); \
	$(AR) mPiT $$($(AR) t $@ | sed -n 1p) $@ $$($(AR) t $@ | grep -F -f $(srctree)/scripts/head-object-list.txt)

# ============================================================================
# if_changed 示例：仅当依赖变化时才重建 vmlinux.a
# 工作流程：
#   1. 检查 $(KBUILD_VMLINUX_OBJS) 中的任何内置目标文件是否更新
#   2. 检查 scripts/head-object-list.txt 是否更新
#   3. 检查上次的 AR 命令行是否与当前相同（通过 .vmlinux.a.cmd 文件）
#   4. 以上任一条件满足 → 执行 $(call cmd,ar_vmlinux.a)
#      并将新命令行保存到 .vmlinux.a.cmd
#   5. 否则 → 跳过（@:），不需要重建
# ============================================================================
targets += vmlinux.a
vmlinux.a: $(KBUILD_VMLINUX_OBJS) scripts/head-object-list.txt FORCE
	$(call if_changed,ar_vmlinux.a)

# vmlinux_o：将 vmlinux.a 和额外库链接为 vmlinux.o（部分链接/可重定位目标文件）
# scripts/Makefile.vmlinux_o 使用 ld -r 进行部分链接，生成一个包含所有内核代码
# 的单一 .o 文件。这样做的好处是：
#   1. 可以在最终的 vmlinux 链接时进行全程序优化（LTO）
#   2. 可以在 modpost 阶段检查所有符号
PHONY += vmlinux_o
vmlinux_o: vmlinux.a $(KBUILD_VMLINUX_LIBS)
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.vmlinux_o

# vmlinux.o 的生成会触发 modules.builtin 的生成（记录内置模块列表）
vmlinux.o modules.builtin.modinfo modules.builtin: vmlinux_o
	@:

# ============================================================================
# vmlinux 最终链接
# ============================================================================
PHONY += vmlinux
# 顶层 Makefile 中的 LDFLAGS_vmlinux 定义顶层 vmlinux 的链接器标志，
# 而不是解压缩器的。arch/*/boot/compressed/vmlinux 中的 LDFLAGS_vmlinux 是无关的。
# 两者的区别：
#   - 顶层 vmlinux：完整的内核 ELF 镜像（可能几十 MB）
#   - boot/compressed/vmlinux：压缩后的启动镜像（通常 < 10MB）
#
# _LDFLAGS_vmlinux 是 'private export' bug 的变通方案：
#   GNU Make 的 'private export' 在目标特定变量中无法正常工作，
#   所以先将 LDFLAGS_vmlinux 私有化存储到 _LDFLAGS_vmlinux，
#   然后再通过 export 传递下去。
#   参考：https://savannah.gnu.org/bugs/?61463
# 对于 Make > 4.4，以下简单代码将工作：
#   vmlinux: private export LDFLAGS_vmlinux := $(LDFLAGS_vmlinux)
vmlinux: private _LDFLAGS_vmlinux := $(LDFLAGS_vmlinux)
vmlinux: export LDFLAGS_vmlinux = $(_LDFLAGS_vmlinux)
vmlinux: vmlinux.o $(KBUILD_LDS) modpost
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.vmlinux

# 防止隐式规则干扰
# 这些目标文件（链接脚本、vmlinux 目标文件、库）在递归下降子目录的过程中
# 被实际的构建规则生成，make 的隐式规则不应该尝试去匹配它们。
# 这里用空规则（: ; 即空 recipe）声明它们已存在规则，防止 make 搜索隐式规则。
$(sort $(KBUILD_LDS) $(KBUILD_VMLINUX_OBJS) $(KBUILD_VMLINUX_LIBS)): . ;

# ============================================================================
# KERNELRELEASE 版本字符串的生成
# ============================================================================
# KERNELRELEASE 是完整的内核版本字符串，例如 "7.1.0-rc3" 或 "7.1.0-rc3-g1a2b3c4d5e6f"。
# 它由两部分组成：
#   1. Makefile 中定义的版本号（VERSION.PATCHLEVEL.SUBLEVEL EXTRAVERSION）
#   2. scripts/setlocalversion 脚本生成的本地版本后缀（如 -g<git-hash>）
#
# 两种读取方式：
#   1. file 来源（首次构建）：通过 setlocalversion 脚本从源码树计算
#      - 检查 git describe 获取最近的标签
#      - 如果树不干净（有未提交的修改），添加 -dirty 后缀
#      - 如果定义了 LOCALVERSION 配置，追加它
#   2. 已有值（后续构建）：直接使用已读取的 KERNELRELEASE
#
# include/config/kernel.release 文件存储计算结果，
# 使用 filechk 机制确保只有内容变化时才更新（避免不必要的重新编译）。
ifeq ($(origin KERNELRELEASE),file)
filechk_kernel.release = $(srctree)/scripts/setlocalversion $(srctree)
else
filechk_kernel.release = echo $(KERNELRELEASE)
endif

# ============================================================================
# filechk 示例：生成 include/config/kernel.release
# filechk 的工作流程：
#   1. 执行 filechk_kernel.release（在 ifeq 分支中已定义），输出写入临时文件
#   2. 比较临时文件与现有文件的内容（cmp -s）
#   3. 如果内容不同或无现有文件 → 用新内容替换（mv）
#   4. 如果内容相同 → 不更新文件，避免触发不必要的重新编译
# 依赖 FORCE 的 .PHONY 目标确保每次都执行检测。
# ============================================================================
include/config/kernel.release: FORCE
	$(call filechk,kernel.release)

# ============================================================================
# scripts 目标：构建 scripts/ 中的辅助工具
# ============================================================================
PHONY += scripts
scripts: scripts_basic scripts_dtc
	$(Q)$(MAKE) $(build)=$(@)

# ============================================================================
# prepare 多级准备流程
# ============================================================================
# 在递归构建内核或模块之前需要完成的事项统一列在 "prepare" 中。
# 采用多级方法（prepare3 → prepare2 → prepare1 → prepare0 → prepare），
# 每级在前一级的基础上增加更多准备工作：
#
#   prepare3（最低级）:
#     - outputmakefile：在输出目录创建简易 Makefile
#     - archheaders/archscripts：架构特定的头文件和脚本生成
#     - scripts_basic：构建 fixdep 等基础工具
#     - include/config/kernel.release：内核版本字符串
#     - asm-generic：生成通用 asm 头文件包装器
#     - include/generated/*.h：版本和编译信息头文件
#     - remove-stale-files：清理旧版本遗留的过期文件
#
#   prepare0（中间级）:
#     - scripts/mod：构建 modpost 和 elfconfig 等模块处理工具
#     - $(build)=. prepare：运行顶层目录的 prepare 目标
#       这会处理 include/config/ 下的配置头文件更新等
#
#   prepare（最高级）:
#     - Rust 工具链可用性检查（如果启用了 CONFIG_RUST）
#     - Rust 内核代码的编译准备工作
#
# archprepare 在架构 Makefile 中被引用，是架构特定的准备入口。
# 实际执行顺序：
#   archprepare → prepare0 → prepare
# 大部分构建依赖只需要 archprepare 即可（确保头文件就绪），
# 但完整的模块构建和外部模块构建需要完整的 prepare。

PHONY += prepare archprepare

archprepare: outputmakefile archheaders archscripts scripts include/config/kernel.release \
	asm-generic $(version_h) include/generated/utsrelease.h \
	include/generated/compile.h include/generated/autoconf.h \
	include/generated/rustc_cfg remove-stale-files

prepare0: archprepare
	$(Q)$(MAKE) $(build)=scripts/mod
	$(Q)$(MAKE) $(build)=. prepare

# 完整的准备流程（包含 Rust 工具链检查）
prepare: prepare0
ifdef CONFIG_RUST
	+$(Q)$(CONFIG_SHELL) $(srctree)/scripts/rust_is_available.sh
	$(Q)$(MAKE) $(build)=rust
endif

# 清理过时的文件（旧版本内核留下的）
PHONY += remove-stale-files
remove-stale-files:
	$(Q)$(srctree)/scripts/remove-stale-files

# ============================================================================
# asm-generic：通用头文件支持
# ============================================================================
# 用于在 asm-generic 中生成架构特定的通用头文件包装器
asm-generic := -f $(srctree)/scripts/Makefile.asm-headers obj

PHONY += asm-generic uapi-asm-generic
asm-generic: uapi-asm-generic
	$(Q)$(MAKE) $(asm-generic)=arch/$(SRCARCH)/include/generated/asm \
	generic=include/asm-generic
uapi-asm-generic:
	$(Q)$(MAKE) $(asm-generic)=arch/$(SRCARCH)/include/generated/uapi/asm \
	generic=include/uapi/asm-generic

# ============================================================================
# 自动生成的头文件
# ============================================================================
# 内核构建过程中自动生成几个关键头文件，提供版本和编译信息。

# ---------------------------------------------------------------------------
# 1. include/generated/uapi/linux/version.h — Linux 版本号宏
#    - LINUX_VERSION_CODE：数值版本号，格式 V*65536 + P*256 + S
#      例如 7.1.0 → 7*65536 + 1*256 + 0 = 458752 + 256 = 459008
#      用于驱动程序的版本兼容性检查 #if LINUX_VERSION_CODE >= KERNEL_VERSION(5,10,0)
#    - KERNEL_VERSION(a,b,c)：生成版本号的宏
#    - LINUX_VERSION_MAJOR / PATCHLEVEL / SUBLEVEL：独立的主/次/子版本宏
#    - SUBLEVEL 被限制为 0-255（如果超过 255 则固定为 255）
define filechk_version.h
	if [ $(SUBLEVEL) -gt 255 ]; then                                 \
		echo \#define LINUX_VERSION_CODE $(shell                 \
		expr $(VERSION) \* 65536 + $(PATCHLEVEL) \* 256 + 255); \
	else                                                             \
		echo \#define LINUX_VERSION_CODE $(shell                 \
		expr $(VERSION) \* 65536 + $(PATCHLEVEL) \* 256 + $(SUBLEVEL)); \
	fi;                                                              \
	echo '#define KERNEL_VERSION(a,b,c) (((a) << 16) + ((b) << 8) +  \
	((c) > 255 ? 255 : (c)))';                                       \
	echo \#define LINUX_VERSION_MAJOR $(VERSION);                    \
	echo \#define LINUX_VERSION_PATCHLEVEL $(PATCHLEVEL);            \
	echo \#define LINUX_VERSION_SUBLEVEL $(SUBLEVEL)
endef

$(version_h): private PATCHLEVEL := $(or $(PATCHLEVEL), 0)
$(version_h): private SUBLEVEL := $(or $(SUBLEVEL), 0)
$(version_h): FORCE
	$(call filechk,version.h)

# ---------------------------------------------------------------------------
# 2. include/generated/utsrelease.h — UTS 发布版本字符串
#    - UTS_RELEASE 宏：定义内核版本字符串（如 "7.1.0-rc3"）
#    - 用于 /proc/version 和 uname -r 命令的输出
#    - UTS 代表 "Unix Time-sharing System"
#    - utsname 结构体的 release 字段长度限制为 65 字节（__NEW_UTS_LEN）
#      这里限制为 64 字符以保留结尾的 NUL 字节
uts_len := 64
define filechk_utsrelease.h
	if [ `echo -n "$(KERNELRELEASE)" | wc -c ` -gt $(uts_len) ]; then \
	  echo '"$(KERNELRELEASE)" exceeds $(uts_len) characters' >&2;    \
	  exit 1;                                                         \
	fi;                                                               \
	echo \#define UTS_RELEASE \"$(KERNELRELEASE)\"
endef

include/generated/utsrelease.h: include/config/kernel.release FORCE
	$(call filechk,utsrelease.h)

# ---------------------------------------------------------------------------
# 3. include/generated/compile.h — 编译信息
#    - 包含编译主机名、编译时间、编译器版本、链接器信息
#    - 用于内核启动时的构建信息显示（/proc/version 的额外信息）
#    - 由 scripts/mkcompile_h 脚本生成，该脚本使用 UTS_MACHINE 和编译器版本来
#      生成一个包含编译上下文的头文件
filechk_compile.h = $(srctree)/scripts/mkcompile_h \
	"$(UTS_MACHINE)" "$(CONFIG_CC_VERSION_TEXT)" "$(LD)"

include/generated/compile.h: FORCE
	$(call filechk,compile.h)

# ============================================================================
# 头文件依赖检查
# ============================================================================
PHONY += headerdep
headerdep:
	$(Q)find $(srctree)/include/ -name '*.h' | xargs --max-args 1 \
	$(srctree)/scripts/headerdep.pl -I$(srctree)/include

# ============================================================================
# 内核头文件安装
# ============================================================================
# 默认头文件安装位置
export INSTALL_HDR_PATH = $(objtree)/usr

quiet_cmd_headers_install = INSTALL $(INSTALL_HDR_PATH)/include
      cmd_headers_install = \
	mkdir -p $(INSTALL_HDR_PATH); \
	rsync -mrl --include='*/' --include='*\.h' --exclude='*' \
	usr/include $(INSTALL_HDR_PATH)

# cmd 示例：执行 headers_install（安装内核 UAPI 头文件）
# 需要事先定义 quiet_cmd_headers_install（简短标签）和 cmd_headers_install（实际命令）。
# 根据 $(quiet) 自动选择输出：无/简短/完整命令。
PHONY += headers_install
headers_install: headers
	$(call cmd,headers_install)

PHONY += archheaders archscripts

hdr-inst := -f $(srctree)/scripts/Makefile.headersinst obj

PHONY += headers
headers: $(version_h) scripts_unifdef uapi-asm-generic archheaders
ifdef HEADER_ARCH
	$(Q)$(MAKE) -f $(srctree)/Makefile HEADER_ARCH= SRCARCH=$(HEADER_ARCH) headers
else
	$(Q)$(MAKE) $(hdr-inst)=include/uapi
	$(Q)$(MAKE) $(hdr-inst)=arch/$(SRCARCH)/include/uapi
endif

ifdef CONFIG_HEADERS_INSTALL
prepare: headers
endif

# ============================================================================
# initramfs/initcpio 生成工具
# ============================================================================
PHONY += usr_gen_init_cpio
usr_gen_init_cpio: scripts_basic
	$(Q)$(MAKE) $(build)=usr usr/gen_init_cpio

PHONY += scripts_unifdef
scripts_unifdef: scripts_basic
	$(Q)$(MAKE) $(build)=scripts scripts/unifdef

PHONY += scripts_gen_packed_field_checks
scripts_gen_packed_field_checks: scripts_basic
	$(Q)$(MAKE) $(build)=scripts scripts/gen_packed_field_checks

# ============================================================================
# 安装
# ============================================================================
# 许多发行版有自定义安装脚本 /sbin/installkernel。
# 如果安装了 DKMS，'make install' 最终会递归回此 Makefile 来构建和安装外部模块。
# 取消 sub_make_done 以便解析 M=, V= 等选项。

quiet_cmd_install = INSTALL $(INSTALL_PATH)
      cmd_install = unset sub_make_done; $(srctree)/scripts/install.sh

# ============================================================================
# vDSO 安装
# ============================================================================
PHONY += vdso_install
vdso_install: export INSTALL_FILES = $(vdso-install-y)
vdso_install:
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.vdsoinst

# ============================================================================
# 工具
# ============================================================================
# objtool：栈验证和 ORC 调试信息生成工具
ifdef CONFIG_OBJTOOL
prepare: tools/objtool
endif

# resolve_btfids：解析 BTF ID 的工具（BPF 需要）
ifdef CONFIG_BPF
ifdef CONFIG_DEBUG_INFO_BTF
prepare: tools/bpf/resolve_btfids
endif
endif

# 工具的构建系统不是 Kbuild 的一部分，往往有其独特的问题。
# 如果需要集成新工具，请考虑将其放在 tools/ 树之外，使用标准 Kbuild
# "hostprogs" 语法，而不是在这里添加新的 tools/* 条目。
# 详见 Documentation/kbuild/makefiles.rst。

PHONY += resolve_btfids_clean

resolve_btfids_O = $(abspath $(objtree))/tools/bpf/resolve_btfids

# 输出目录中可能没有 tools/bpf/resolve_btfids 目录，跳过清理
resolve_btfids_clean:
ifneq ($(wildcard $(resolve_btfids_O)),)
	$(Q)$(MAKE) -sC $(srctree)/tools/bpf/resolve_btfids O=$(resolve_btfids_O) clean
endif

PHONY += objtool_clean objtool_mrproper

objtool_O = $(abspath $(objtree))/tools/objtool

objtool_clean objtool_mrproper:
ifneq ($(wildcard $(objtool_O)),)
	$(Q)$(MAKE) -sC $(abs_srctree)/tools/objtool O=$(objtool_O) srctree=$(abs_srctree) $(patsubst objtool_%,%,$@)
endif

tools/: FORCE
	$(Q)mkdir -p $(objtree)/tools
	$(Q)$(MAKE) O=$(abspath $(objtree)) subdir=tools -C $(srctree)/tools/

tools/%: FORCE
	$(Q)mkdir -p $(objtree)/tools
	$(Q)$(MAKE) O=$(abspath $(objtree)) subdir=tools -C $(srctree)/tools/ $*

# ============================================================================
# 内核自测试（kselftest）
# ============================================================================
PHONY += kselftest
kselftest: headers
	$(Q)$(MAKE) -C $(srctree)/tools/testing/selftests run_tests

kselftest-%: headers FORCE
	$(Q)$(MAKE) -C $(srctree)/tools/testing/selftests $*

PHONY += kselftest-merge
kselftest-merge:
	$(if $(wildcard $(objtree)/.config),, $(error No .config exists, config your kernel first!))
	$(Q)find $(srctree)/tools/testing/selftests -name config -o -name config.$(UTS_MACHINE) | \
		xargs $(srctree)/scripts/kconfig/merge_config.sh -y -m $(objtree)/.config
	$(Q)$(MAKE) -f $(srctree)/Makefile olddefconfig

# ============================================================================
# 设备树（Devicetree）文件
# ============================================================================
ifneq ($(wildcard $(srctree)/arch/$(SRCARCH)/boot/dts/),)
dtstree := arch/$(SRCARCH)/boot/dts
endif

dtbindingtree := Documentation/devicetree/bindings

%.yaml: dtbs_prepare
	$(Q)$(MAKE) $(build)=$(dtbindingtree) \
		    $(dtbindingtree)/$(patsubst %.yaml,%.example.dtb,$@) dt_binding_check_one

ifneq ($(dtstree),)

# 设备树 blob (.dtb) 和设备树覆盖层 (.dtbo) 的构建规则
%.dtb: dtbs_prepare
	$(Q)$(MAKE) $(build)=$(dtstree) $(dtstree)/$@

%.dtbo: dtbs_prepare
	$(Q)$(MAKE) $(build)=$(dtstree) $(dtstree)/$@

PHONY += dtbs dtbs_prepare dtbs_install dtbs_check
dtbs: dtbs_prepare
	$(Q)$(MAKE) $(build)=$(dtstree) need-dtbslist=1

# dtbs_prepare 需要 kernel.release，因为 INSTALL_DTBS_PATH 包含 $(KERNELRELEASE)
dtbs_prepare: include/config/kernel.release scripts_dtc

ifneq ($(filter dtbs_check %.yaml, $(MAKECMDGOALS)),)
export CHECK_DTBS=y
endif

ifneq ($(CHECK_DTBS),)
dtbs_prepare: dt_binding_schemas
endif

dtbs_check: dtbs

dtbs_install:
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.dtbinst obj=$(dtstree)

ifdef CONFIG_OF_EARLY_FLATTREE
all: dtbs
endif

ifdef CONFIG_GENERIC_BUILTIN_DTB
vmlinux: dtbs
endif

endif

# DTC（Device Tree Compiler）脚本
PHONY += scripts_dtc
scripts_dtc: scripts_basic
	$(Q)$(MAKE) $(build)=scripts/dtc

ifneq ($(filter dt_binding_check, $(MAKECMDGOALS)),)
export CHECK_DTBS=y
endif

PHONY += dt_binding_check dt_binding_schemas
dt_binding_check: dt_binding_schemas scripts_dtc
	$(Q)$(MAKE) $(build)=$(dtbindingtree) $@

dt_binding_schemas:
	$(Q)$(MAKE) $(build)=$(dtbindingtree)

PHONY += dt_compatible_check
dt_compatible_check: dt_binding_schemas
	$(Q)$(MAKE) $(build)=$(dtbindingtree) $@

# ============================================================================
# 模块（Modules）
# ============================================================================
ifdef CONFIG_MODULES

# 默认也构建模块
all: modules

# 当使用 modversions 构建模块时，也需要在下降过程中考虑内置对象，
# 以确保在记录校验和之前它们是最新的。
ifdef CONFIG_MODVERSIONS
  KBUILD_BUILTIN := y
endif

# *.ko 通常独立于 vmlinux，但 CONFIG_DEBUG_INFO_BTF_MODULES 例外
ifdef CONFIG_DEBUG_INFO_BTF_MODULES
KBUILD_BUILTIN := y
modules: vmlinux
endif

modules: modules_prepare

# 准备构建外部模块（生成 module.lds 链接脚本）
modules_prepare: prepare
	$(Q)$(MAKE) $(build)=scripts scripts/module.lds

endif # CONFIG_MODULES

# ============================================================================
# 清理规则（Cleaning）—— 三级清理体系
# ============================================================================
# Linux 内核的清理分为三个级别，每个级别比前一个更彻底：
#
#   make clean — 第一级（编译产物清理）
#     - 删除所有编译生成的 .o, .a, .ko 等目标文件
#     - 删除 vmlinux 相关的链接产物（vmlinux.o, System.map 等）
#     - 删除模块相关的中间文件（modules.order, .mod.c 等）
#     - 保留 .config 和足够的构建基础设施，使得外部模块仍可编译
#     - 典型用途：切换编译选项后重新编译、清理编译中间产物
#
#   make mrproper — 第二级（配置+编译产物清理）
#     - 包含 clean 的所有操作
#     - 额外删除 .config 和所有配置生成的文件（include/config/ 等）
#     - 额外删除架构生成的头文件（arch/*/include/generated/）
#     - 额外删除密钥文件、打包产物等
#     - 典型用途：完全重置构建环境、切换架构前的清理
#     - "mr" 代表 "make remove"，proper 表示"彻底"
#
#   make distclean — 第三级（发行级清理）
#     - 包含 mrproper 的所有操作
#     - 额外删除编辑器备份文件（*.orig, *.rej, *~, *.bak）
#     - 额外删除补丁残留文件、标签文件、cscope 索引等
#     - 典型用途：准备源代码打包发布（make dist）
#     - 清理后的目录应该只包含受版本控制的原文件

# 'make clean' 删除的目录和文件
CLEAN_FILES += vmlinux.symvers modules-only.symvers \
	       modules.builtin modules.builtin.modinfo modules.nsdeps \
	       modules.builtin.ranges vmlinux.o.map vmlinux.unstripped \
	       compile_commands.json rust/test \
	       rust-project.json .vmlinux.objs .vmlinux.export.c \
               .builtin-dtbs-list .builtin-dtbs.S

# 'make mrproper' 删除的目录和文件（包括 .config 和所有生成的头文件）
MRPROPER_FILES += include/config include/generated          \
		  arch/$(SRCARCH)/include/generated .objdiff \
		  debian snap tar-install PKGBUILD pacman \
		  .config .config.old .version \
		  Module.symvers \
		  certs/signing_key.pem \
		  certs/x509.genkey \
		  vmlinux-gdb.py \
		  rpmbuild \
		  rust/libmacros.so rust/libmacros.dylib \
		  rust/libpin_init_internal.so rust/libpin_init_internal.dylib

# clean：删除大部分文件，但保留足够的构建外部模块
clean: private rm-files := $(CLEAN_FILES)

PHONY += archclean vmlinuxclean

vmlinuxclean:
	$(Q)$(CONFIG_SHELL) $(srctree)/scripts/link-vmlinux.sh clean
	$(Q)$(if $(ARCH_POSTLINK), $(MAKE) -f $(ARCH_POSTLINK) clean)

clean: archclean vmlinuxclean resolve_btfids_clean objtool_clean

# mrproper：删除所有生成的文件，包括 .config
mrproper: private rm-files := $(MRPROPER_FILES)
mrproper-dirs      := $(addprefix _mrproper_,scripts)

PHONY += $(mrproper-dirs) mrproper
$(mrproper-dirs):
	$(Q)$(MAKE) $(clean)=$(patsubst _mrproper_%,%,$@)

mrproper: clean objtool_mrproper $(mrproper-dirs)
	$(call cmd,rmfiles)
	@find . $(RCS_FIND_IGNORE) \
		\( -name '*.rmeta' \) \
		-type f -print | xargs rm -f

# distclean：在 mrproper 基础上删除编辑器备份和补丁残留文件
PHONY += distclean

distclean: mrproper
	@find . $(RCS_FIND_IGNORE) \
		\( -name '*.orig' -o -name '*.rej' -o -name '*~' \
		-o -name '*.bak' -o -name '#*#' -o -name '*%' \
		-o -name 'core' -o -name tags -o -name TAGS -o -name 'cscope*' \
		-o -name GPATH -o -name GRTAGS -o -name GSYMS -o -name GTAGS \) \
		-type f -print | xargs rm -f

# ============================================================================
# 内核打包（Packaging）
# ============================================================================
modules-cpio-pkg: usr_gen_init_cpio

%src-pkg: FORCE
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.package $@
%pkg: include/config/kernel.release FORCE
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.package $@

# ============================================================================
# 帮助信息（make help）
# ============================================================================
boards := $(wildcard $(srctree)/arch/$(SRCARCH)/configs/*_defconfig)
boards := $(sort $(notdir $(boards)))
board-dirs := $(dir $(wildcard $(srctree)/arch/$(SRCARCH)/configs/*/*_defconfig))
board-dirs := $(sort $(notdir $(board-dirs:/=)))

PHONY += help
help:
	@echo  'Cleaning targets:'
	@echo  '  clean		  - Remove most generated files but keep the config and'
	@echo  '                    enough build support to build external modules'
	@echo  '  mrproper	  - Remove all generated files + config + various backup files'
	@echo  '  distclean	  - mrproper + remove editor backup and patch files'
	@echo  ''
	@$(MAKE) -f $(srctree)/scripts/kconfig/Makefile help
	@echo  ''
	@echo  'Other generic targets:'
	@echo  '  all		  - Build all targets marked with [*]'
	@echo  '* vmlinux	  - Build the bare kernel'
	@echo  '* modules	  - Build all modules'
	@echo  '  modules_install - Install all modules to INSTALL_MOD_PATH (default: /)'
	@echo  '  vdso_install    - Install unstripped vdso to INSTALL_MOD_PATH (default: /)'
	@echo  '  dir/            - Build all files in dir and below'
	@echo  '  dir/file.[ois]  - Build specified target only'
	@echo  '  dir/file.ll     - Build the LLVM assembly file'
	@echo  '                    (requires compiler support for LLVM assembly generation)'
	@echo  '  dir/file.lst    - Build specified mixed source/assembly target only'
	@echo  '                    (requires a recent binutils and recent build (System.map))'
	@echo  '  dir/file.ko     - Build module including final link'
	@echo  '  modules_prepare - Set up for building external modules'
	@echo  '  tags/TAGS	  - Generate tags file for editors'
	@echo  '  cscope	  - Generate cscope index'
	@echo  '  gtags           - Generate GNU GLOBAL index'
	@echo  '  kernelrelease	  - Output the release version string (use with make -s)'
	@echo  '  kernelversion	  - Output the version stored in Makefile (use with make -s)'
	@echo  '  image_name	  - Output the image name (use with make -s)'
	@echo  '  headers	  - Build ready-to-install UAPI headers in usr/include'
	@echo  '  headers_install - Install sanitised kernel UAPI headers to INSTALL_HDR_PATH'; \
	 echo  '                    (default: $(INSTALL_HDR_PATH))'; \
	 echo  ''
	@echo  'Static analysers:'
	@echo  '  checkstack      - Generate a list of stack hogs and consider all functions'
	@echo  '                    with a stack size larger than MINSTACKSIZE (default: 100)'
	@echo  '  versioncheck    - Sanity check on version.h usage'
	@echo  '  includecheck    - Check for duplicate included header files'
	@echo  '  headerdep       - Detect inclusion cycles in headers'
	@echo  '  coccicheck      - Check with Coccinelle'
	@echo  '  clang-analyzer  - Check with clang static analyzer'
	@echo  '  clang-tidy      - Check with clang-tidy'
	@echo  ''
	@echo  'Tools:'
	@echo  '  nsdeps          - Generate missing symbol namespace dependencies'
	@echo  ''
	@echo  'Kernel selftest:'
	@echo  '  kselftest         - Build and run kernel selftest'
	@echo  '                      Build, install, and boot kernel before'
	@echo  '                      running kselftest on it'
	@echo  '                      Run as root for full coverage'
	@echo  '  kselftest-all     - Build kernel selftest'
	@echo  '  kselftest-install - Build and install kernel selftest'
	@echo  '  kselftest-clean   - Remove all generated kselftest files'
	@echo  '  kselftest-merge   - Merge all the config dependencies of'
	@echo  '		      kselftest to existing .config.'
	@echo  ''
	@echo  'Rust targets:'
	@echo  '  rustavailable   - Checks whether the Rust toolchain is'
	@echo  '		    available and, if not, explains why.'
	@echo  '  rustfmt	  - Reformat all the Rust code in the kernel'
	@echo  '  rustfmtcheck	  - Checks if all the Rust code in the kernel'
	@echo  '		    is formatted, printing a diff otherwise.'
	@echo  '  rustdoc	  - Generate Rust documentation'
	@echo  '		    (requires kernel .config)'
	@echo  '  rusttest        - Runs the Rust tests'
	@echo  '                    (requires kernel .config; downloads external repos)'
	@echo  '  rust-analyzer	  - Generate rust-project.json rust-analyzer support file'
	@echo  '		    (requires kernel .config)'
	@echo  '  dir/file.[os]   - Build specified target only'
	@echo  '  dir/file.rsi    - Build macro expanded source, similar to C preprocessing.'
	@echo  '                    Run with RUSTFMT=n to skip reformatting if needed.'
	@echo  '                    The output is not intended to be compilable.'
	@echo  '  dir/file.ll     - Build the LLVM assembly file'
	@echo  ''
	@$(if $(dtstree), \
		echo 'Devicetree:'; \
		echo '* dtbs               - Build device tree blobs for enabled boards'; \
		echo '  dtbs_install       - Install dtbs to $(INSTALL_DTBS_PATH)'; \
		echo '  dt_binding_check   - Validate device tree binding documents and examples'; \
		echo '  dt_binding_schemas - Build processed device tree binding schemas'; \
		echo '  dtbs_check         - Validate device tree source files';\
		echo '')

	@echo 'Userspace tools targets:'
	@echo '  use "make tools/help"'
	@echo '  or  "cd tools; make help"'
	@echo  ''
	@echo  'Kernel packaging:'
	@$(MAKE) -f $(srctree)/scripts/Makefile.package help
	@echo  ''
	@echo  'Documentation targets:'
	@$(MAKE) -f $(srctree)/Documentation/Makefile dochelp
	@echo  ''
	@echo  'Architecture-specific targets ($(SRCARCH)):'
	@$(or $(archhelp),\
		echo '  No architecture-specific help defined for $(SRCARCH)')
	@echo  ''
	@$(if $(boards), \
		$(foreach b, $(boards), \
		printf "  %-27s - Build for %s\\n" $(b) $(subst _defconfig,,$(b));) \
		echo '')
	@$(if $(board-dirs), \
		$(foreach b, $(board-dirs), \
		printf "  %-16s - Show %s-specific targets\\n" help-$(b) $(b);) \
		printf "  %-16s - Show all of the above\\n" help-boards; \
		echo '')

	@echo  '  make V=n   [targets] 1: verbose build'
	@echo  '                       2: give reason for rebuild of target'
	@echo  '                       V=1 and V=2 can be combined with V=12'
	@echo  '  make O=dir [targets] Locate all output files in "dir", including .config'
	@echo  '  make C=1   [targets] Check re-compiled c source with $$CHECK'
	@echo  '                       (sparse by default)'
	@echo  '  make C=2   [targets] Force check of all c source with $$CHECK'
	@echo  '  make RECORDMCOUNT_WARN=1 [targets] Warn about ignored mcount sections'
	@echo  '  make W=n   [targets] Enable extra build checks, n=1,2,3,c,e where'
	@echo  '		1: warnings which may be relevant and do not occur too often'
	@echo  '		2: warnings which occur quite often but may still be relevant'
	@echo  '		3: more obscure warnings, can most likely be ignored'
	@echo  '		c: extra checks in the configuration stage (Kconfig)'
	@echo  '		e: warnings are being treated as errors'
	@echo  '		Multiple levels can be combined with W=12 or W=123'
	@echo  '  make UT=1   [targets] Warn if a tracepoint is defined but not used.'
	@echo  '          [ This will be removed when all current unused tracepoints are eliminated. ]'
	@$(if $(dtstree), \
		echo '  make CHECK_DTBS=1 [targets] Check all generated dtb files against schema'; \
		echo '         This can be applied both to "dtbs" and to individual "foo.dtb" targets' ; \
		)
	@echo  ''
	@echo  'Execute "make" or "make all" to build all targets marked with [*] '
	@echo  'For further info see the ./README file'


help-board-dirs := $(addprefix help-,$(board-dirs))

help-boards: $(help-board-dirs)

boards-per-dir = $(sort $(notdir $(wildcard $(srctree)/arch/$(SRCARCH)/configs/$*/*_defconfig)))

$(help-board-dirs): help-%:
	@echo  'Architecture-specific targets ($(SRCARCH) $*):'
	@$(if $(boards-per-dir), \
		$(foreach b, $(boards-per-dir), \
		printf "  %-24s - Build for %s\\n" $*/$(b) $(subst _defconfig,,$(b));) \
		echo '')

# ============================================================================
# 文档目标
# ============================================================================
DOC_TARGETS := xmldocs latexdocs pdfdocs htmldocs epubdocs cleandocs \
	       linkcheckdocs dochelp refcheckdocs texinfodocs infodocs mandocs \
	       htmldocs-redirects

PHONY += $(DOC_TARGETS)
$(DOC_TARGETS):
	$(Q)$(MAKE) $(build)=Documentation $@

# ============================================================================
# Rust 目标
# ============================================================================

# "Rust 是否可用？" 目标
PHONY += rustavailable
rustavailable:
	+$(Q)$(CONFIG_SHELL) $(srctree)/scripts/rust_is_available.sh && echo "Rust is available!"

# Rust 文档目标
PHONY += rustdoc
rustdoc: prepare
	$(Q)$(MAKE) $(build)=rust $@

# Rust 测试目标
PHONY += rusttest
rusttest: prepare
	$(Q)$(MAKE) $(build)=rust $@

# Rust 格式化目标
PHONY += rustfmt rustfmtcheck

rustfmt:
	$(Q)find $(srctree) $(RCS_FIND_IGNORE) \
		\( \
			-path $(srctree)/rust/proc-macro2 \
			-o -path $(srctree)/rust/quote \
			-o -path $(srctree)/rust/syn \
		\) -prune -o \
		-type f -a -name '*.rs' -a ! -name '*generated*' -print \
		| xargs $(RUSTFMT) $(rustfmt_flags)

rustfmtcheck: rustfmt_flags = --check
rustfmtcheck: rustfmt

# ============================================================================
# 杂项
# ============================================================================
PHONY += misc-check
misc-check:
	$(Q)$(srctree)/scripts/misc-check

all: misc-check

# GDB 脚本支持
PHONY += scripts_gdb
scripts_gdb: prepare0
	$(Q)$(MAKE) $(build)=scripts/gdb
	$(Q)ln -fsn $(abspath $(srctree)/scripts/gdb/vmlinux-gdb.py)

ifdef CONFIG_GDB_SCRIPTS
all: scripts_gdb
endif

else # KBUILD_EXTMOD

# ============================================================================
# 外部模块构建支持（KBUILD_EXTMOD）
# ============================================================================
# 外部模块是指位于内核源码树之外的第三方模块（如驱动程序）。
# 使用 "make M=/path/to/module" 或 "make -C /path/to/kernel M=$PWD" 构建。
#
# 外部模块构建的关键特点：
#   1. 内核源码视为只读——不修改内核源码树中的任何文件
#   2. 只构建模块（KBUILD_BUILTIN 清空，KBUILD_MODULES 强制为 y）
#   3. 使用已构建内核的配置（从内核构建目录读取 auto.conf）
#   4. 编译器版本和 pahole 版本检查：确保与构建内核时使用的工具链一致
#      - 如果编译器版本不匹配，仅警告但不阻止（大多数情况兼容）
#      - 如果 pahole 版本不匹配，仅警告（可能影响 BTF 调试信息）
#   5. Module.symvers 文件：从内核构建目录导入，提供内核导出符号的校验和信息
#
# 构建流程：
#   1. prepare：验证工具链版本
#   2. $(build)=$@：使用 scripts/Makefile.build 编译模块目录
#   3. Makefile.modpost：处理模块符号依赖和版本检查（modpost）
#   4. Makefile.modfinal：最终链接生成 .ko 文件
#
# 如果基础内核需要更新（如配置变化或重新编译），必须使用普通 make 命令
# （不带 M=...）在内核源码树中先构建新内核。

filechk_kernel.release = echo $(KERNELRELEASE)

# 外部模块始终只构建模块（不构建内置对象）
KBUILD_BUILTIN :=
KBUILD_MODULES := y

build-dir := .

clean-dirs := .
clean: private rm-files := Module.symvers modules.nsdeps compile_commands.json

PHONY += prepare
# 现在展开为简单变量以减少 shell 评估的成本
prepare: CC_VERSION_TEXT := $(CC_VERSION_TEXT)
prepare: PAHOLE_VERSION := $(PAHOLE_VERSION)
prepare:
	@if [ "$(CC_VERSION_TEXT)" != "$(CONFIG_CC_VERSION_TEXT)" ]; then \
		echo >&2 "warning: the compiler differs from the one used to build the kernel"; \
		echo >&2 "  The kernel was built by: $(CONFIG_CC_VERSION_TEXT)"; \
		echo >&2 "  You are using:           $(CC_VERSION_TEXT)"; \
	fi
	@if [ "$(PAHOLE_VERSION)" != "$(CONFIG_PAHOLE_VERSION)" ]; then \
		echo >&2 "warning: pahole version differs from the one used to build the kernel"; \
		echo >&2 "  The kernel was built with: $(CONFIG_PAHOLE_VERSION)"; \
		echo >&2 "  You are using:             $(PAHOLE_VERSION)"; \
	fi

PHONY += help
help:
	@echo  '  Building external modules.'
	@echo  '  Syntax: make -C path/to/kernel/src M=$$PWD target'
	@echo  ''
	@echo  '  modules         - default target, build the module(s)'
	@echo  '  modules_install - install the module'
	@echo  '  clean           - remove generated files in module directory only'
	@echo  '  rust-analyzer	  - generate rust-project.json rust-analyzer support file'
	@echo  ''

ifndef CONFIG_MODULES
modules modules_install: __external_modules_error
__external_modules_error:
	@echo >&2 '***'
	@echo >&2 '*** The present kernel disabled CONFIG_MODULES.'
	@echo >&2 '*** You cannot build or install external modules.'
	@echo >&2 '***'
	@false
endif

endif # KBUILD_EXTMOD

# ============================================================================
# 模块（Modules）—— 构建规则
# ============================================================================

PHONY += modules modules_install modules_sign modules_prepare

modules_install:
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.modinst \
	sign-only=$(if $(filter modules_install,$(MAKECMDGOALS)),,y)

ifeq ($(CONFIG_MODULE_SIG),y)
# modules_sign 是 modules_install 的子集
# 'make modules_install modules_sign' 等同于 'make modules_install'
modules_sign: modules_install
	@:
else
modules_sign:
	@echo >&2 '***'
	@echo >&2 '*** CONFIG_MODULE_SIG is disabled. You cannot sign modules.'
	@echo >&2 '***'
	@false
endif

ifdef CONFIG_MODULES

modules.order: $(build-dir)
	@:

# KBUILD_MODPOST_NOFINAL 可以设置为跳过模块的最终链接。
# 这仅用于加速测试编译。
modules: modpost
ifneq ($(KBUILD_MODPOST_NOFINAL),1)
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.modfinal
endif

PHONY += modules_check
modules_check: modules.order
	$(Q)$(CONFIG_SHELL) $(srctree)/scripts/modules-check.sh $<

else # CONFIG_MODULES

modules:
	@:

KBUILD_MODULES :=

endif # CONFIG_MODULES

# ============================================================================
# modpost：模块后处理（符号解析与校验）
# ============================================================================
# modpost 是内核模块构建的关键后处理步骤，由 scripts/Makefile.modpost 驱动。
# 它在 vmlinux.o（如果构建内置）和模块编译之后运行，负责：
#
#   1. 符号依赖解析：
#      - 检查每个模块引用的符号是否由内核（vmlinux）或其他模块导出
#      - 生成模块间的依赖关系（记录在 modules.dep 中）
#      - 如果模块引用的符号在任何地方都没有定义，报告错误
#
#   2. 模块版本检查（CONFIG_MODVERSIONS）：
#      - 为每个导出的符号计算 CRC 校验和
#      - 加载模块时验证校验和，确保模块与内核二进制兼容
#      - 校验和变化意味着 ABI 变化，模块需要重新编译
#
#   3. 导出符号处理：
#      - 生成 Module.symvers 文件（所有导出符号的列表和校验和）
#      - 外部模块构建时使用此文件来解析符号依赖
#
#   4. 模块别名和许可信息：
#      - 处理 MODULE_ALIAS, MODULE_LICENSE 等宏
#      - 生成 modules.alias, modules.builtin 等文件
#
#   5. 生成 .mod.c 文件：
#      - 为每个模块创建一个包含模块元数据的 C 文件
#      - 这些 .mod.c 文件在最终链接时编译并链接到 .ko 中
#
# modpost 的依赖：
#   - 如果构建内置：需要 vmlinux.o（包含所有内核符号）
#   - 如果构建模块：需要 modules_check（验证 modules.order 一致性）
PHONY += modpost
modpost: $(if $(single-build),, $(if $(KBUILD_BUILTIN), vmlinux.o)) \
	 $(if $(KBUILD_MODULES), modules_check)
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.modpost

# ============================================================================
# 单一目标构建
# ============================================================================
# 构建子目录中的单个文件，例如:
#   make foo/bar/baz.s
# 支持的后缀列在 'single-targets' 中。
#
# 只构建特定子目录下的目标:
#   make foo/bar/baz/

ifdef single-build

# .ko 是特殊的，因为需要 modpost
single-ko := $(sort $(filter %.ko, $(MAKECMDGOALS)))
single-no-ko := $(filter-out $(single-ko), $(MAKECMDGOALS)) \
		$(foreach x, o mod, $(patsubst %.ko, %.$x, $(single-ko)))

$(single-ko): single_modules
	@:
$(single-no-ko): $(build-dir)
	@:

# 完成后删除 modules.order，因为它不是真正的
PHONY += single_modules
single_modules: $(single-no-ko) modules_prepare
	$(Q){ $(foreach m, $(single-ko), echo $(m:%.ko=%.o);) } > modules.order
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.modpost
ifneq ($(KBUILD_MODPOST_NOFINAL),1)
	$(Q)$(MAKE) -f $(srctree)/scripts/Makefile.modfinal
endif
	$(Q)rm -f modules.order

single-goals := $(addprefix $(build-dir)/, $(single-no-ko))

KBUILD_MODULES := y

endif

prepare: outputmakefile

# ============================================================================
# 递归构建入口
# ============================================================================
# 预设语言环境变量以加速构建过程。
# 将语言环境调整限制在此处，避免运行 make menuconfig 等时的错误语言设置。
# 错误消息仍以原始语言显示。
PHONY += $(build-dir)
$(build-dir): prepare
	$(Q)$(MAKE) $(build)=$@ need-builtin=1 need-modorder=1 $(single-goals)

# ============================================================================
# 清理规则（通用部分）—— $(clean)= 的使用
# $(clean)=dir 展开为：-f scripts/Makefile.clean obj=dir
# Makefile.clean 处理该目录的清理逻辑，删除 .o, .a, .ko 等文件。
# 这里使用 _clean_ 前缀的伪目标来触发各目录的清理。
# 例如 _clean_scripts → $(clean)=scripts
# ============================================================================
clean-dirs := $(addprefix _clean_, $(clean-dirs))
PHONY += $(clean-dirs) clean
$(clean-dirs):
	$(Q)$(MAKE) $(clean)=$(patsubst _clean_%,%,$@)

clean: $(clean-dirs)
	$(call cmd,rmfiles)
	@find . $(RCS_FIND_IGNORE) \
		\( -name '*.[aios]' -o -name '*.rsi' -o -name '*.ko' -o -name '.*.cmd' \
		-o -name '*.ko.*' \
		-o -name '*.dtb' -o -name '*.dtbo' \
		-o -name '*.dtb.S' -o -name '*.dtbo.S' \
		-o -name '*.dt.yaml' -o -name 'dtbs-list' \
		-o -name '*.dwo' -o -name '*.lst' \
		-o -name '*.su' -o -name '*.mod' \
		-o -name '.*.d' -o -name '.*.tmp' -o -name '*.mod.c' \
		-o -name '*.lex.c' -o -name '*.tab.[ch]' \
		-o -name '*.asn1.[ch]' \
		-o -name '*.symtypes' -o -name 'modules.order' \
		-o -name '*.c.[012]*.*' \
		-o -name '*.ll' \
		-o -name '*.gcno' \
		\) -type f -print \
		-o -name '.tmp_*' -print \
		| xargs rm -rf

# ============================================================================
# cmd 示例：生成编辑器标签文件（tags, TAGS, cscope, gtags）
# cmd_tags 定义为执行 scripts/tags.sh，传入目标名作为参数（$@）。
# 脚本根据目标名选择生成 ctags/etags/cscope/gtags 索引。
# ============================================================================
quiet_cmd_tags = GEN     $@
      cmd_tags = $(BASH) $(srctree)/scripts/tags.sh $@

tags TAGS cscope gtags: FORCE
	$(call cmd,tags)

# ============================================================================
# rust-analyzer 支持
# ============================================================================
# 生成 rust-project.json（描述非 Cargo Rust 项目结构），供 rust-analyzer 使用
PHONY += rust-analyzer
rust-analyzer:
	+$(Q)$(CONFIG_SHELL) $(srctree)/scripts/rust_is_available.sh
ifdef KBUILD_EXTMOD
	$(Q)$(MAKE) $(build)=$(objtree)/rust src=$(srctree)/rust $@
else
	$(Q)$(MAKE) $(build)=rust $@
endif

# ============================================================================
# nsdeps：生成缺失的符号命名空间依赖
# ============================================================================
PHONY += nsdeps
nsdeps: export KBUILD_NSDEPS=1
nsdeps: modules
	$(Q)$(CONFIG_SHELL) $(srctree)/scripts/nsdeps

# ============================================================================
# Clang 工具
# ============================================================================
quiet_cmd_gen_compile_commands = GEN     $@
      cmd_gen_compile_commands = $(PYTHON3) $< -a $(AR) -o $@ $(filter-out $<, $(real-prereqs))

# ============================================================================
# if_changed 示例：生成 compile_commands.json（LSP 编译数据库）
# 该文件供 clangd、clang-tidy、clang-analyzer 等工具使用，
# 记录每个源文件的编译命令。仅当 vmlinux.a 或 modules.order 变化时重建。
# 生成命令定义在 cmd_gen_compile_commands 中。
# ============================================================================
compile_commands.json: $(srctree)/scripts/clang-tools/gen_compile_commands.py \
	$(if $(KBUILD_EXTMOD),, vmlinux.a $(KBUILD_VMLINUX_LIBS)) \
	$(if $(CONFIG_MODULES), modules.order) FORCE
	$(call if_changed,gen_compile_commands)

targets += compile_commands.json

PHONY += clang-tidy clang-analyzer

ifdef CONFIG_CC_IS_CLANG
quiet_cmd_clang_tools = CHECK   $<
      cmd_clang_tools = $(PYTHON3) $(srctree)/scripts/clang-tools/run-clang-tools.py $@ $<

clang-tidy clang-analyzer: compile_commands.json
	$(call cmd,clang_tools)
else
clang-tidy clang-analyzer:
	@echo "$@ requires CC=clang" >&2
	@false
endif

# ============================================================================
# 一致性检查脚本
# ============================================================================
PHONY += includecheck versioncheck coccicheck

# includecheck：检查重复包含的头文件
includecheck:
	find $(srctree)/* $(RCS_FIND_IGNORE) \
		-name '*.[hcS]' -type f -print | sort \
		| xargs $(PERL) -w $(srctree)/scripts/checkincludes.pl

# versioncheck：检查 version.h 使用的一致性
versioncheck:
	find $(srctree)/* $(RCS_FIND_IGNORE) \
		-name '*.[hcS]' -type f -print | sort \
		| xargs $(PERL) -w $(srctree)/scripts/checkversion.pl

# coccicheck：使用 Coccinelle 进行语义补丁检查
coccicheck:
	$(Q)$(BASH) $(srctree)/scripts/$@

# ============================================================================
# 栈使用检查、版本信息、镜像名称
# ============================================================================
PHONY += checkstack kernelrelease kernelversion image_name

# UML（用户模式 Linux）需要特殊处理：使用主机工具链，所以需要 $(SUBARCH)
# 其他架构（包括交叉编译）使用 $(ARCH)
ifeq ($(ARCH), um)
CHECKSTACK_ARCH := $(SUBARCH)
else
CHECKSTACK_ARCH := $(ARCH)
endif
# 默认最小栈大小阈值
MINSTACKSIZE	?= 100
checkstack:
	$(OBJDUMP) -d vmlinux $$(find . -name '*.ko') | \
	$(PERL) $(srctree)/scripts/checkstack.pl $(CHECKSTACK_ARCH) $(MINSTACKSIZE)

kernelrelease:
	@$(filechk_kernel.release)

kernelversion:
	@echo $(KERNELVERSION)

image_name:
	@echo $(KBUILD_IMAGE)

PHONY += run-command
run-command:
	$(Q)$(KBUILD_RUN_COMMAND)

# ============================================================================
# cmd 示例：清理规则中使用的 rmfiles
# quiet_cmd_rmfiles 仅在 $(rm-files) 列表中的文件存在时才显示 "CLEAN" 标签。
# cmd_rmfiles 用 rm -rf 删除这些文件。
# 在 clean 和 mrproper 中通过 $(call cmd,rmfiles) 调用，
# 配合 private rm-files := ... 传递要删除的文件列表。
# ============================================================================
quiet_cmd_rmfiles = $(if $(wildcard $(rm-files)),CLEAN   $(wildcard $(rm-files)))
      cmd_rmfiles = rm -rf $(rm-files)

# ============================================================================
# 读取已保存的命令行（用于 if_changed 系列函数）
# ============================================================================
# .*.cmd 文件存储了上次构建的命令行，用于检测是否需要重新构建
existing-targets := $(wildcard $(sort $(targets)))

-include $(foreach f,$(existing-targets),$(dir $(f)).$(notdir $(f)).cmd)

endif # config-build
endif # mixed-build
endif # need-sub-make

# ============================================================================
# FORCE 目标
# ============================================================================
# FORCE 是一个始终"过期"的伪目标，用于强制某些规则始终执行
PHONY += FORCE
FORCE:

# 将 PHONY 变量中声明的所有目标标记为 .PHONY
.PHONY: $(PHONY)
