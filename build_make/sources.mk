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
	kernel/tkernel/inttarget.c \
	kernel/tkernel/klk.c \
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
	peripheral_kernel/sysman/gpt.c \
	peripheral_kernel/fs/fs.c \
	peripheral_kernel/fs/pipefs.c \
	peripheral_kernel/fs/devfs.c \
	peripheral_kernel/fs/objfs.c \
	peripheral_kernel/fs/fatfs.c \
	peripheral_kernel/fs/exfatfs.c 	peripheral_kernel/fs/ntfsfs.c \
	peripheral_kernel/fs/fsname.c \
	peripheral_kernel/fs/fsprobe.c \
	peripheral_kernel/fs/crc32c.c \
	peripheral_kernel/fs/tsfs.c \
	peripheral_kernel/fs/tsfsblk.c \
	peripheral_kernel/fs/tsfspart.c \
	peripheral_kernel/fs/tsfsbtree.c \
	peripheral_kernel/fs/tsfsjrnl.c \
	peripheral_kernel/fs/tsfsobj.c \
	peripheral_kernel/fs/tsfsck.c \
	peripheral_kernel/obj/objson.c \
	peripheral_kernel/obj/obprot.c \
	peripheral_kernel/obj/obhash.c \
	peripheral_kernel/obj/obname.c \
	peripheral_kernel/obj/obfile.c \
	peripheral_kernel/obj/obfsck.c \
	peripheral_kernel/obj/obxref.c \
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
	peripheral_kernel/sysman/uuidtext.c \
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
	outer_kernel/tad/tad_lnkvol.c \
	outer_kernel/tad/tad_edit.c \
	outer_kernel/tad/tad_pat.c \
	outer_kernel/tad/tad_view.c \
	application/fig/figgeom.c

SRCS_LIBOM := \
	outer_kernel/om/om.c \
	outer_kernel/om/om_vdraw.c \
	outer_kernel/om/om_store.c \
	outer_kernel/om/om_copy.c \
	outer_kernel/om/om_obj.c \
	outer_kernel/xf/xf.c

# 画像の復号と PNG の符号化。画面の無い機械でも、実身のアイコンや絵を読み書きする
SRCS_LIBIMG := \
	outer_kernel/img/inflate.c \
	outer_kernel/img/deflate.c \
	outer_kernel/img/png.c \
	outer_kernel/img/img_scale.c \
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

# GPU の共通の層、プロセスからの SVC(gp_)と V3D のアドレス空間(第13章)。機械に依らない
# ので両方の機械で建て、QEMU では試験(ktest_gpu.c、ktest_gl.c)が使う。V3D そのものを
# 動かす側は rpi5.mk が足す
SRCS_DEVICE  := \
	device/gpu/gpu.c \
	device/gpu/gpu_svc.c \
	device/ser/ser_svc.c \
	device/gpu/v3d/v3d_mmu.c \
	device/i2c/i2c_gpio.c \
	device/i2c/gt911.c

# i2c_gpio.c と gt911.c は、ピンを動かす I2C と GT911(設計書 6.4.2)。機械に依らないので
# 全部の機械で建てる。Raspberry Pi 4 のボード側(tpanel.c)が使い、試験(ktest_tpanel.c)は
# 偽のピンと偽の GT911 でどの機械でも確かめる

# 画面の機械に依らない側・描画・窓・書体。画面を得る側(disp_bochs.c、disp_rpi5.c)は
# 各 .mk が足す。画面の見つからない機械では起動時に何も描かずに済ませる(設計書 16.5)
SRCS_GUI := \
	device/disp/disp.c \
	outer_kernel/dp/dp.c \
	outer_kernel/dp/dp_pat.c \
	outer_kernel/dp/dp_rgn.c \
	outer_kernel/dp/dp_shape.c \
	outer_kernel/wm/look.c \
	outer_kernel/wm/wm.c \
	outer_kernel/wm/compose.c \
	outer_kernel/wm/compose_simd.c \
	outer_kernel/wm/part.c \
	outer_kernel/wm/keymap.c \
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
	outer_kernel/wm/fquit.c \
	outer_kernel/print/pdf.c \
	outer_kernel/print/pdf_font.c \
	outer_kernel/print/prn_rec.c \
	outer_kernel/print/print.c \
	$(SRCS_FONT)

# デスクトップとカーネルの中のアプリ、かな漢字変換(設計書 16.5.12、17.14)
SRCS_DESKTOP := \
	application/cab/cab.c \
	application/cab/cabmenu.c \
	application/doc/docview.c \
	application/doc/docpaint.c \
	application/doc/docmenu.c \
	application/doc/docfind.c \
	application/doc/docedit.c \
	application/kconv/kconv.c \
	lib/libudic/udic.c \
	application/desktop/desktop.c \
	application/desktop/dtedit.c \
	application/desktop/dtcab.c \
	application/desktop/dtform.c \
	application/desktop/dtcolour.c \
	application/desktop/dtprint.c \
	application/desktop/dtvmn.c \
	application/desktop/dtinfo.c \
	application/desktop/dtfault.c \
	application/desktop/dtpkg.c \
	application/desktop/dtacc.c \
	application/desktop/dtsysup.c \
	application/desktop/dtuitest.c \
	application/desktop/dtsys.c \
	application/desktop/dttray.c \
	application/desktop/dtprog.c \
	application/desktop/dtres.c \
	application/desktop/dttool.c \
	application/desktop/dtscr.c \
	application/desktop/dttrayv.c \
	application/desktop/dtnotify.c \
	application/desktop/dtfix.c \
	application/sysinst/sysinst.c \
	application/sysinst/si_core.c \
	application/desktop/dtdisk.c \
	application/desktop/dttype.c \
	application/desktop/dtadmin.c \
	application/desktop/dtdoc.c \
	application/desktop/dtfig.c \
	application/fig/figedit.c \
	application/fig/figtool.c \
	application/fig/figcanvas.c \
	application/fig/figpat.c \
	application/desktop/dtftool.c \
	application/desktop/dtfcanvas.c \
	application/desktop/dtfpat.c \
	application/desktop/dttip.c \
	application/desktop/dtslide.c \
	application/slide/sl_deck.c \
	application/desktop/dtrt.c \
	application/desktop/dtrthost.c \
	lib/librtad/rt_num.c \
	lib/librtad/rt_prog.c \
	lib/librtad/rt_run.c \
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
	peripheral_kernel/network/netbsd/so_wg.c \
	peripheral_kernel/network/netbsd/so_dhcp.c \
	peripheral_kernel/network/netbsd/so_slaac.c \
	peripheral_kernel/network/netbsd/so_dns.c
endif

SRCS_APP := \
	application/app_main.c

ifeq ($(TESTAGENT),1)
SRCS_DESKTOP += application/desktop/dttagent.c
endif

# Bluetooth(設計書 6.8)。LE のペアリングの P-256 と AES は lib/mbedtls の一部をカーネルの旗で作る
SRCS_BT := device/bt/bt.c device/bt/bt_le.c device/bt/bt_a2dp.c device/bt/bt_sdp.c device/bt/bt_avrcp.c device/bt/bt_rfcomm.c device/bt/bt_hfp.c device/bt/bt_crypto.c device/bt/sbc_enc.c device/bt/bt_intel.c \
	device/bt/bt_h4.c device/bt/bt_bcm.c \
	lib/mbedtls/library/aes.c lib/mbedtls/library/bignum.c lib/mbedtls/library/bignum_core.c \
	lib/mbedtls/library/constant_time.c lib/mbedtls/library/ecp.c lib/mbedtls/library/ecp_curves.c \
	lib/mbedtls/library/platform_util.c
BT_MBED_FLAGS := -w '-DMBEDTLS_CONFIG_FILE="bt_mbed_config.h"' -I$(TOP)/device/bt -I$(TOP)/lib/mbedtls/include
BT_MBED_OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(filter lib/mbedtls/%,$(SRCS_BT)) device/bt/bt_crypto.c)
# LE Audio(設計書 6.8)。LC3 の符号化は lib/liblc3 を変更せずに使う。浮動小数点で計算するので、
# このファイル群だけ浮動小数点のレジスタを使ってよいように翻訳する(makefile の LC3_OBJS)
SRCS_LC3 := $(addprefix lib/liblc3/src/,attdet.c bits.c bwdet.c energy.c lc3.c ltpf.c mdct.c plc.c sns.c spec.c tables.c tns.c) \
	device/bt/lc3_fmath.c
SRCS_BT += device/bt/bt_iso.c device/bt/bt_bap.c device/bt/bt_lea.c $(SRCS_LC3)
LC3_FLAGS := -w -fno-math-errno -I$(TOP)/lib/liblc3/include
LC3_OBJS = $(patsubst %.c,$(OBJDIR)/%.o,$(SRCS_LC3))

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
	tests/ktest/ktest_klk.c \
	tests/ktest/ktest_lockup.c \
	tests/ktest/ktest_blk.c \
	tests/ktest/ktest_sd.c \
	tests/ktest/ktest_board.c \
	tests/ktest/ktest_ser.c \
	tests/ktest/ktest_fs.c \
	tests/ktest/ktest_fat.c \
	tests/ktest/ktest_tsfscut.c \
	tests/ktest/ktest_proc.c \
	tests/ktest/ktest_prcobj.c \
	tests/ktest/ktest_pan.c \
	tests/ktest/ktest_stkg.c \
	tests/ktest/ktest_cred.c \
	tests/ktest/ktest_cxx.c \
	tests/ktest/ktest_v8.c \
	tests/ktest/ktest_uuid.c \
	tests/ktest/ktest_tsfs.c \
	tests/ktest/ktest_tsfsblk.c \
	tests/ktest/ktest_tsfsbtree.c \
	tests/ktest/ktest_tsfsjrnl.c \
	tests/ktest/ktest_tsfsobj.c \
	tests/ktest/ktest_iob.c \
	tests/ktest/ktest_nicring.c \
	tests/ktest/ktest_dfix.c \
	tests/ktest/ktest_ob.c \
	tests/ktest/ktest_tad.c \
	tests/ktest/ktest_om.c \
	tests/ktest/ktest_usb.c \
	tests/ktest/ktest_digi.c \
	tests/ktest/ktest_tpanel.c \
	tests/ktest/ktest_devob.c \
	tests/ktest/ktest_snd.c \
	tests/ktest/ktest_cam.c \
	tests/ktest/ktest_store.c \
	tests/ktest/ktest_sysup.c \
	tests/ktest/ktest_unp.c \
	lib/libbpk/bpk_lh5.c \
	tests/ktest/ktest_arc.c \
	lib/libbpk/bpk_arc.c \
	lib/libbpk/bpk_lh5e.c \
	lib/libbpk/bpk_pack.c \
	tests/ktest/ktest_btbk.c \
	tests/ktest/ktest_btbk_data.S \
	tests/ktest/ktest_speech_data.S \
	tests/ktest/ktest_bkp.c \
	lib/libbtbk/btbk_lzss.c \
	lib/libbtbk/btbk_arc.c \
	lib/libbtbk/btbk_tad.c \
	lib/libbpk/bpk_tron.c \
	lib/libbpk/bpk_jis.c \
	lib/libbpk/bpk_rt.c \
	lib/libbpk/bpk_tad.c \
	lib/libbpk/bpk_util.c \
	lib/libbpk/bpk_png.c \
	lib/libbpk/bpk_fax.c \
	tests/ktest/ktest_rt.c \
	tests/ktest/ktest_tray.c \
	tests/ktest/ktest_xf.c \
	tests/ktest/ktest_xfu.c \
	tests/ktest/ktest_btcd.c \
	tests/ktest/ktest_vols.c \
	tests/ktest/ktest_grow.c \
	lib/libbtfs/btfs.c \
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
	tests/ktest/ktest_rump.c \
	tests/ktest/ktest_svcio.c \
	tests/ktest/ktest_wg.c \
	tests/ktest/ktest_psx.c \
	tests/ktest/ktest_disp.c \
	tests/ktest/ktest_dp.c \
	tests/ktest/ktest_wm.c \
	tests/ktest/ktest_cab.c \
	tests/ktest/ktest_tv.c \
	tests/ktest/ktest_vt.c \
	tests/ktest/ktest_prn.c \
	tests/ktest/ktest_scr.c \
	tests/ktest/ktest_part.c \
	tests/ktest/ktest_mn.c \
	tests/ktest/ktest_ms.c \
	tests/ktest/ktest_xfc.c \
	tests/ktest/ktest_drop.c \
	tests/ktest/ktest_dtwin.c \
	tests/ktest/ktest_fault.c \
	tests/ktest/ktest_fq.c \
	tests/ktest/ktest_fn.c \
	tests/ktest/ktest_gpu.c \
	tests/ktest/ktest_gl.c \
	tests/ktest/ktest_cal.c \
	application/calendar/cal_tad.c \
	tests/ktest/ktest_calc.c \
	application/calc/calc_tad.c \
	application/calc/calc_print.c \
	tests/ktest/ktest_udic.c \
	tests/ktest/ktest_bt.c \
	tests/ktest/ktest_btuart.c \
	tests/ktest/ktest_emu.c \
	tests/ktest/ktest_kana.c \
	application/kanatype/kp_eng.c \
	lib/libkbmap/kbmap.c \
	lib/libkbmap/kbmap_tab.c \
	tests/ktest/ktest_calcu.c \
	application/calculator/cu_dec.c \
	application/calculator/cu_eng.c \
	tests/ktest/ktest_km.c \
	tests/ktest/ktest_skb.c \
	tests/ktest/ktest_mcard.c \
	application/mcard/mc_db.c \
	application/mcard/mc_bin.c \
	application/mcard/mc_rx.c \
	application/mcard/mc_var.c \
	application/mcard/mc_io.c \
	application/mcard/mc_merge.c \
	application/mcard/mc_prn.c \
	tests/ktest/ktest_techo.c \
	application/techo/tc_db.c \
	lib/libui/ui_prn.c \
	tests/ktest/ktest_chs.c \
	application/charsearch/cs_dic.c \
	application/charsearch/cs_code.c \
	tests/ktest/ktest_slide.c

# 汎用の PCI Express の構成空間(平らな ECAM)を歩く組。それを持つ機械
# (qemu_virt.mk)だけが足す。持たない機械では ktest_main.c が SKIP と出す
SRCS_KTEST_QEMU := \
	tests/ktest/ktest_pcie.c
endif

# コア依存。ARCH_CORE(makefile)の置き場のものを選ぶ。ボード依存は各 .mk で足す
# AArch64(armv8a)
SRCS_CORE_armv8a := \
	kernel/sysdepend/cpu/core/armv8a/boot.S \
	kernel/sysdepend/cpu/core/armv8a/vector.S \
	kernel/sysdepend/cpu/core/armv8a/dispatch.S \
	kernel/sysdepend/cpu/core/armv8a/int_asm.S \
	kernel/sysdepend/cpu/core/armv8a/interrupt.c \
	kernel/sysdepend/cpu/core/armv8a/exc_hdl.c \
	kernel/sysdepend/cpu/core/armv8a/cpu_cntl.c \
	kernel/sysdepend/cpu/core/armv8a/reset_main.c \
	kernel/sysdepend/cpu/core/armv8a/dtb.c \
	kernel/sysdepend/cpu/core/armv8a/board.c \
	kernel/sysdepend/cpu/core/armv8a/mmu.c \
	kernel/sysdepend/cpu/core/armv8a/sys_timer.c \
	kernel/sysdepend/cpu/core/armv8a/smp.c \
	kernel/sysdepend/cpu/core/armv8a/lockup.c \
	kernel/sysdepend/cpu/core/armv8a/cpuclk.c \
	lib/libtk/sysdepend/cpu/core/armv8a/int_armv8a.c \
	lib/libtk/sysdepend/cpu/core/armv8a/spinlock.c \
	lib/libtk/sysdepend/cpu/core/armv8a/wusec_armv8a.c \
	lib/libtm/sysdepend/pl011/tm_com.c

# x86-64(x86_64)
SRCS_CORE_x86_64 := \
	kernel/sysdepend/cpu/core/x86_64/boot.S \
	kernel/sysdepend/cpu/core/x86_64/vector.S \
	kernel/sysdepend/cpu/core/x86_64/dispatch.S \
	kernel/sysdepend/cpu/core/x86_64/tramp.S \
	kernel/sysdepend/cpu/core/x86_64/pm_ctx.S \
	kernel/sysdepend/cpu/core/x86_64/interrupt.c \
	kernel/sysdepend/cpu/core/x86_64/exc_hdl.c \
	kernel/sysdepend/cpu/core/x86_64/syscall.c \
	kernel/sysdepend/cpu/core/x86_64/cpu_cntl.c \
	kernel/sysdepend/cpu/core/x86_64/reset_main.c \
	kernel/sysdepend/cpu/core/x86_64/board.c \
	kernel/sysdepend/cpu/core/x86_64/mmu.c \
	kernel/sysdepend/cpu/core/x86_64/apic.c \
	kernel/sysdepend/cpu/core/x86_64/sys_timer.c \
	kernel/sysdepend/cpu/core/x86_64/smp.c \
	kernel/sysdepend/cpu/core/x86_64/lockup.c \
	kernel/sysdepend/cpu/core/x86_64/cpuclk.c \
	lib/libtk/sysdepend/cpu/core/x86_64/int_x86_64.c \
	lib/libtk/sysdepend/cpu/core/x86_64/spinlock.c \
	lib/libtk/sysdepend/cpu/core/x86_64/wusec_x86_64.c \
	lib/libtm/sysdepend/ns16550/tm_com.c

SRCS_CORE := $(SRCS_CORE_$(ARCH_CORE))

SRCS := $(SRCS_TKERNEL) $(SRCS_SYSMAN) $(SRCS_SYSINIT) \
	$(SRCS_LIBTK) $(SRCS_LIBTM) $(SRCS_LIBTAD) $(SRCS_LIBOM) $(SRCS_LIBIMG) $(SRCS_TRAY) $(SRCS_LIBJSON) $(SRCS_LIBCONF) \
	$(SRCS_DEVICE) $(SRCS_GUI) $(SRCS_DESKTOP) $(SRCS_NET) \
	$(SRCS_APP) $(SRCS_CORE)
