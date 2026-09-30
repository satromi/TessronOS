#
# sources.mk — ターゲット非依存のソース一覧(TOP からの相対パス)
#
#   ボード・SoC・コア依存のソースは rpi5.mk / qemu_virt.mk 側で SRCS に足す。
#

SRCS_TKERNEL := \
	kernel/tkernel/cpuctl.c \
	kernel/tkernel/device.c \
	kernel/tkernel/deviceio.c \
	kernel/tkernel/eventflag.c \
	kernel/tkernel/int.c \
	kernel/tkernel/klock.c \
	kernel/tkernel/mailbox.c \
	kernel/tkernel/memory.c \
	kernel/tkernel/mempfix.c \
	kernel/tkernel/mempool.c \
	kernel/tkernel/messagebuf.c \
	kernel/tkernel/misc_calls.c \
	kernel/tkernel/mutex.c \
	kernel/tkernel/objname.c \
	kernel/tkernel/owner.c \
	kernel/tkernel/power.c \
	kernel/tkernel/rendezvous.c \
	kernel/tkernel/semaphore.c \
	kernel/tkernel/task.c \
	kernel/tkernel/task_manage.c \
	kernel/tkernel/task_sync.c \
	kernel/tkernel/time_calls.c \
	kernel/tkernel/timer.c \
	kernel/tkernel/tkinit.c \
	kernel/tkernel/wait.c

SRCS_SYSMAN := \
	peripheral_kernel/sysman/pfalloc.c \
	peripheral_kernel/sysman/vmap.c \
	peripheral_kernel/sysman/smem.c \
	peripheral_kernel/sysman/space.c \
	peripheral_kernel/sysman/part.c \
	peripheral_kernel/fs/fs.c \
	peripheral_kernel/fs/fatfs.c \
	peripheral_kernel/fs/crc32c.c \
	peripheral_kernel/fs/tsfs.c \
	peripheral_kernel/fs/tsfsblk.c \
	peripheral_kernel/fs/tsfsbtree.c \
	peripheral_kernel/fs/tsfsjrnl.c \
	peripheral_kernel/fs/tsfsobj.c \
	peripheral_kernel/obj/objson.c \
	peripheral_kernel/obj/obprot.c \
	peripheral_kernel/obj/obhash.c \
	peripheral_kernel/obj/obname.c \
	peripheral_kernel/obj/obfile.c \
	peripheral_kernel/obj/obmem.c \
	peripheral_kernel/obj/obdev.c \
	peripheral_kernel/obj/obsys.c \
	peripheral_kernel/obj/obrand.c \
	peripheral_kernel/obj/obdisp.c \
	peripheral_kernel/obj/obinput.c \
	peripheral_kernel/obj/obusb.c \
	peripheral_kernel/obj/obgpio.c \
	peripheral_kernel/obj/obprc.c \
	peripheral_kernel/obj/obchan.c \
	peripheral_kernel/obj/obsock.c \
	peripheral_kernel/obj/obuser.c \
	peripheral_kernel/obj/obsvc.c \
	peripheral_kernel/sysman/proc.c \
	peripheral_kernel/sysman/umem.c \
	peripheral_kernel/sysman/elf.c \
	peripheral_kernel/sysman/pcie.c \
	peripheral_kernel/sysman/rng.c \
	peripheral_kernel/sysman/datetime.c \
	peripheral_kernel/sysman/svc_gate.c \
	peripheral_kernel/sysman/svc_tk.c

SRCS_SYSINIT := \
	kernel/sysinit/sysinit.c \
	kernel/inittask/inittask.c \
	kernel/usermain/usermain.c \
	kernel/tstdlib/bitop.c \
	kernel/tstdlib/string.c \
	kernel/tstdlib/memfunc.c

SRCS_LIBTK := \
	lib/libtk/kmalloc.c \
	lib/libtk/pool.c \
	lib/libtk/fastlock.c \
	lib/libtk/fastmlock.c

SRCS_LIBTM := \
	lib/libtm/libtm.c \
	lib/libtm/libtm_printf.c

# xmlTAD と実身/仮身のユーザ側ライブラリ(設計書 16.3.3、16.4.1)
SRCS_LIBTAD := \
	outer_kernel/tad/tad.c \
	outer_kernel/tad/tad_link.c \
	outer_kernel/tad/tad_edit.c \
	outer_kernel/tad/tad_pat.c \
	outer_kernel/tad/tad_view.c \
	application/fig/figgeom.c

SRCS_LIBOM := \
	outer_kernel/om/om.c \
	outer_kernel/om/om_store.c \
	outer_kernel/om/om_copy.c \
	outer_kernel/om/om_obj.c \
	outer_kernel/xf/xf.c

# 画像の復号と PNG の符号化。画面の無い機械でも、実身のアイコンや絵を読み書きする
SRCS_LIBIMG := \
	outer_kernel/img/inflate.c \
	outer_kernel/img/png.c \
	outer_kernel/img/jpeg.c \
	outer_kernel/img/bmp.c \
	outer_kernel/img/gif.c

# トレー。レコードを持つだけで描かないので、窓の層の無い機械にも置く(設計書 18.16)
SRCS_TRAY := \
	outer_kernel/wm/tray.c

# JSON の読み取り。カーネルのどの層も、プロセスのプログラムも使う(設計書 18.15)
SRCS_LIBJSON := \
	lib/libjson/json.c

# 設定のテキスト。カーネルも設定の小物も使う(設計書 16.5.23)
SRCS_LIBCONF := \
	lib/libconf/conf.c

# 書体。FreeType は上流のまま取り込み、受けは outer_kernel/font/port/(設計書 16.6)
FT_DIR := outer_kernel/font/freetype/src
SRCS_FT := \
	$(FT_DIR)/base/ftbase.c \
	$(FT_DIR)/base/ftinit.c \
	$(FT_DIR)/base/ftdebug.c \
	$(FT_DIR)/base/ftbitmap.c \
	$(FT_DIR)/base/ftglyph.c \
	$(FT_DIR)/base/ftmm.c \
	$(FT_DIR)/truetype/truetype.c \
	$(FT_DIR)/sfnt/sfnt.c \
	$(FT_DIR)/smooth/smooth.c \
	$(FT_DIR)/raster/raster.c \
	$(FT_DIR)/cff/cff.c \
	$(FT_DIR)/psaux/psaux.c \
	$(FT_DIR)/pshinter/pshinter.c \
	$(FT_DIR)/psnames/psnames.c

SRCS_LIBC := peripheral_kernel/libc/libc.c peripheral_kernel/libc/setjmp.S

SRCS_FONT := $(SRCS_FT) \
	outer_kernel/font/port/ftsystem.c \
	outer_kernel/font/fn.c $(SRCS_LIBC)

# GPU の共通の層と V3D のアドレス空間(第13章)。機械に依らないので両方の機械で建て、
# QEMU では試験(ktest_gpu.c)が使う。V3D そのものを動かす側は rpi5.mk が足す
SRCS_DEVICE  := \
	device/gpu/gpu.c \
	device/gpu/v3d/v3d_mmu.c

# 画面の機械に依らない側・描画・窓・書体。画面を得る側(disp_bochs.c、disp_rpi5.c)は
# 各 .mk が足す。画面の見つからない機械では起動時に何も描かずに済ませる(設計書 16.5)
SRCS_GUI := \
	device/disp/disp.c \
	outer_kernel/dp/dp.c \
	outer_kernel/dp/dp_rgn.c \
	outer_kernel/dp/dp_shape.c \
	outer_kernel/wm/look.c \
	outer_kernel/wm/wm.c \
	outer_kernel/wm/part.c \
	outer_kernel/wm/pdraw.c \
	outer_kernel/wm/pndef.c \
	outer_kernel/wm/dbox.c \
	outer_kernel/wm/pict.c \
	outer_kernel/wm/pointer.c \
	outer_kernel/wm/msgline.c \
	outer_kernel/wm/uconf.c \
	outer_kernel/wm/beep.c \
	outer_kernel/wm/wmobj.c \
	outer_kernel/wm/mn.c \
	$(SRCS_FONT)

# デスクトップとカーネルの中のアプリ、かな漢字変換(設計書 16.5.12、17.14)
SRCS_DESKTOP := \
	application/cab/cab.c \
	application/cab/cabmenu.c \
	application/doc/docview.c \
	application/doc/docmenu.c \
	application/doc/docfind.c \
	application/kconv/kconv.c \
	application/desktop/desktop.c \
	application/desktop/dtedit.c \
	application/desktop/dtcab.c \
	application/desktop/dtform.c \
	application/desktop/dtvmn.c \
	application/desktop/dtinfo.c \
	application/desktop/dtfault.c \
	application/desktop/dtpkg.c \
	application/desktop/dtuitest.c \
	application/desktop/dtsys.c \
	application/desktop/dttray.c \
	application/desktop/dtprog.c \
	application/desktop/dtres.c \
	application/desktop/dttool.c \
	application/desktop/dtdoc.c \
	application/desktop/dtfig.c \
	application/desktop/dtftool.c \
	application/desktop/dtfcanvas.c \
	application/desktop/dtfpat.c \
	application/desktop/dttip.c \
	application/fig/figview.c

# lwIP と、その TessronOS 側の受け(設計書 12.6)。
# lwIP の一覧は展開で作る: vendor したまま触らないため。
LWIP_DIR := peripheral_kernel/network/lwip/src
SRCS_LWIP := $(patsubst $(TOP)/%,%,\
	$(wildcard $(TOP)/$(LWIP_DIR)/core/*.c) \
	$(wildcard $(TOP)/$(LWIP_DIR)/core/ipv4/*.c) \
	$(wildcard $(TOP)/$(LWIP_DIR)/api/*.c) \
	$(wildcard $(TOP)/$(LWIP_DIR)/netif/ethernet.c))

# NETSTACK=netbsd では、スタックそのものは rump カーネルのオブジェクト(common.mk の
# NETBSD_OBJ)で、ここにあるのはその受け(ハイパーコール、カードとのインタフェース、so_)
SRCS_NET := \
	peripheral_kernel/network/so_prc.c \
	peripheral_kernel/network/so_sntp.c \
	peripheral_kernel/network/so_conf.c
ifeq ($(NETSTACK),lwip)
SRCS_NET += $(SRCS_LWIP) \
	peripheral_kernel/network/port/sys_arch.c \
	peripheral_kernel/network/port/lwip_libc.c \
	peripheral_kernel/network/port/netif_tf.c \
	peripheral_kernel/network/so_api.c
else
SRCS_NET += \
	peripheral_kernel/network/netbsd/rumpuser.c \
	peripheral_kernel/network/netbsd/neta_user.c \
	peripheral_kernel/network/netbsd/so_api.c \
	peripheral_kernel/network/netbsd/so_dhcp.c \
	peripheral_kernel/network/netbsd/so_dns.c
endif

SRCS_APP := \
	application/app_main.c

# カーネル内テスト(make KTEST=1)
ifeq ($(KTEST),1)
SRCS_APP += \
	tests/ktest/ktest_main.c \
	tests/ktest/ktest_task.c \
	tests/ktest/ktest_sync.c \
	tests/ktest/ktest_time.c \
	tests/ktest/ktest_mem.c \
	tests/ktest/ktest_el0.c \
	tests/ktest/ktest_smp.c \
	tests/ktest/ktest_blk.c \
	tests/ktest/ktest_sd.c \
	tests/ktest/ktest_ser.c \
	tests/ktest/ktest_fs.c \
	tests/ktest/ktest_fat.c \
	tests/ktest/ktest_tsfscut.c \
	tests/ktest/ktest_proc.c \
	tests/ktest/ktest_prcobj.c \
	tests/ktest/ktest_cxx.c \
	tests/ktest/ktest_v8.c \
	tests/ktest/ktest_uuid.c \
	tests/ktest/ktest_tsfs.c \
	tests/ktest/ktest_tsfsblk.c \
	tests/ktest/ktest_tsfsbtree.c \
	tests/ktest/ktest_tsfsjrnl.c \
	tests/ktest/ktest_tsfsobj.c \
	tests/ktest/ktest_ob.c \
	tests/ktest/ktest_tad.c \
	tests/ktest/ktest_om.c \
	tests/ktest/ktest_usb.c \
	tests/ktest/ktest_devob.c \
	tests/ktest/ktest_snd.c \
	tests/ktest/ktest_store.c \
	tests/ktest/ktest_unp.c \
	lib/libbpk/bpk_lh5.c \
	tests/ktest/ktest_btbk.c \
	tests/ktest/ktest_btbk_data.S \
	tests/ktest/ktest_bkp.c \
	lib/libbtbk/btbk_lzss.c \
	lib/libbtbk/btbk_arc.c \
	lib/libbtbk/btbk_tad.c \
	lib/libbpk/bpk_tron.c \
	lib/libbpk/bpk_jis.c \
	tests/ktest/ktest_tray.c \
	tests/ktest/ktest_xf.c \
	tests/ktest/ktest_xfu.c \
	lib/libtxc/txc.c \
	lib/libtxc/txc_tab.c \
	lib/libftp/ftp.c \
	lib/libxfu/xfu_util.c \
	lib/libxfu/xfu_name.c \
	lib/libxfu/xfu_obj.c \
	lib/libxfu/xfu_imp.c \
	lib/libxfu/xfu_exp.c \
	lib/libxfu/xfu_fs.c \
	lib/libxfu/xfu_ftp.c \
	lib/libxfu/xfu_knl.c \
	tests/ktest/ktest_bt2.c \
	tests/ktest/ktest_net.c \
	tests/ktest/ktest_so.c \
	tests/ktest/ktest_svcio.c \
	tests/ktest/ktest_disp.c \
	tests/ktest/ktest_dp.c \
	tests/ktest/ktest_wm.c \
	tests/ktest/ktest_cab.c \
	tests/ktest/ktest_tv.c \
	tests/ktest/ktest_part.c \
	tests/ktest/ktest_mn.c \
	tests/ktest/ktest_ms.c \
	tests/ktest/ktest_xfc.c \
	tests/ktest/ktest_drop.c \
	tests/ktest/ktest_fault.c \
	tests/ktest/ktest_fn.c \
	tests/ktest/ktest_gpu.c

# 汎用の PCI Express の構成空間(平らな ECAM)を歩く組。それを持つ機械
# (qemu_virt.mk)だけが足す。持たない機械では ktest_main.c が SKIP と出す
SRCS_KTEST_QEMU := \
	tests/ktest/ktest_pcie.c
endif

# コア依存(AArch64)。ボード依存は各 .mk で足す
SRCS_CORE := \
	kernel/sysdepend/cpu/core/armv8a/boot.S \
	kernel/sysdepend/cpu/core/armv8a/vector.S \
	kernel/sysdepend/cpu/core/armv8a/dispatch.S \
	kernel/sysdepend/cpu/core/armv8a/int_asm.S \
	kernel/sysdepend/cpu/core/armv8a/interrupt.c \
	kernel/sysdepend/cpu/core/armv8a/exc_hdl.c \
	kernel/sysdepend/cpu/core/armv8a/cpu_cntl.c \
	kernel/sysdepend/cpu/core/armv8a/reset_main.c \
	kernel/sysdepend/cpu/core/armv8a/dtb.c \
	kernel/sysdepend/cpu/core/armv8a/mmu.c \
	kernel/sysdepend/cpu/core/armv8a/sys_timer.c \
	kernel/sysdepend/cpu/core/armv8a/smp.c \
	lib/libtk/sysdepend/cpu/core/armv8a/int_armv8a.c \
	lib/libtk/sysdepend/cpu/core/armv8a/spinlock.c \
	lib/libtk/sysdepend/cpu/core/armv8a/wusec_armv8a.c \
	lib/libtm/sysdepend/pl011/tm_com.c

SRCS := $(SRCS_TKERNEL) $(SRCS_SYSMAN) $(SRCS_SYSINIT) \
	$(SRCS_LIBTK) $(SRCS_LIBTM) $(SRCS_LIBTAD) $(SRCS_LIBOM) $(SRCS_LIBIMG) $(SRCS_TRAY) $(SRCS_LIBJSON) $(SRCS_LIBCONF) \
	$(SRCS_DEVICE) $(SRCS_GUI) $(SRCS_DESKTOP) $(SRCS_NET) \
	$(SRCS_APP) $(SRCS_CORE)
