# 付録A　SVCの一覧

プロセスから呼べるシステムコールの一覧である。機能コードは(クラス << 16) | 番号で、`include/ts/svc.h`に定義がある。呼出し方法は5.6を参照。

## クラス0　μT-Kernel

| 番号 | システムコール |
| --- | --- |
| 1〜19 | `tk_cre_tsk`、`tk_del_tsk`、`tk_sta_tsk`、`tk_ext_tsk`、`tk_exd_tsk`、`tk_ter_tsk`、`tk_dis_dsp`、`tk_ena_dsp`、`tk_chg_pri`、`tk_rot_rdq`、`tk_rel_wai`、`tk_get_tid`、`tk_ref_tsk`、`tk_sus_tsk`、`tk_rsm_tsk`、`tk_frsm_tsk`、`tk_slp_tsk`、`tk_wup_tsk`、`tk_can_wup` |
| 20〜24 | `tk_cre_sem`、`tk_del_sem`、`tk_sig_sem`、`tk_wai_sem`、`tk_ref_sem` |
| 25〜30 | `tk_cre_flg`、`tk_del_flg`、`tk_set_flg`、`tk_clr_flg`、`tk_wai_flg`、`tk_ref_flg` |
| 31〜35 | `tk_cre_mbx`、`tk_del_mbx`、`tk_snd_mbx`、`tk_rcv_mbx`、`tk_ref_mbx` |
| 36〜40 | `tk_cre_mtx`、`tk_del_mtx`、`tk_loc_mtx`、`tk_unl_mtx`、`tk_ref_mtx` |
| 41〜45 | `tk_cre_mbf`、`tk_del_mbf`、`tk_snd_mbf`、`tk_rcv_mbf`、`tk_ref_mbf` |
| 46〜49 | `tk_dly_tsk`、`tk_get_tim`、`tk_set_tim`、`tk_get_otm` |
| 50〜59 | `tk_cre_cyc`、`tk_del_cyc`、`tk_sta_cyc`、`tk_stp_cyc`、`tk_ref_cyc`、`tk_cre_alm`、`tk_del_alm`、`tk_sta_alm`、`tk_stp_alm`、`tk_ref_alm` |
| 60〜74 | マイクロ秒単位の版。`tk_slp_tsk_u`、`tk_dly_tsk_u`、`tk_wai_sem_u`、`tk_wai_flg_u`、`tk_rcv_mbx_u`、`tk_loc_mtx_u`、`tk_snd_mbf_u`、`tk_rcv_mbf_u`、`tk_cre_cyc_u`、`tk_ref_cyc_u`、`tk_sta_alm_u`、`tk_ref_alm_u`、`tk_set_tim_u`、`tk_get_tim_u`、`tk_get_otm_u` |
| 75〜77 | `tk_set_utc`、`tk_get_utc`、`tk_get_prc` |

## クラス1　プロセス、メモリ、メッセージ

| 番号 | システムコール | 機能 |
| --- | --- | --- |
| 1 | `ts_get_mono` | 単調時刻 |
| 2 | `ts_cre_prc` | プロセスを生成する |
| 3 | `ts_ext_prc` | 自プロセスを終了する |
| 4 | `ts_ter_prc` | ほかのプロセスを強制終了する |
| 5 | `ts_wai_prc` | 子プロセスの終了を待つ |
| 6 | `ts_ref_prc` | プロセスの状態 |
| 7 | `ts_get_pid` | 自プロセスのID |
| 8 | `ts_gen_uuid` | UUIDを生成する |
| 9 | `ts_get_random` | 乱数。以前に作ったプログラムのために残し、呼んだプロセスの資格で乱数の実身を読む。ライブラリの`ts_get_random`は実身のキーを保持して直接読む |
| 10、11 | `ts_snd_msg`、`ts_rcv_msg` | プロセスメッセージ |
| 12 | `ts_get_arg` | 起動時の引数 |
| 13〜15 | `ts_map_mem`、`ts_unm_mem`、`ts_ctl_mem` | メモリをマッピングする、解除する、属性を変える |

## クラス2　ファイル(FAT)

| 番号 | システムコール |
| --- | --- |
| 1〜10 | `fs_open`、`fs_close`、`fs_read`、`fs_write`、`fs_lseek`、`fs_stat`、`fs_fstat`、`fs_ftruncate`、`fs_truncate`、`fs_getdents` |
| 11〜16 | `fs_mkdir`、`fs_rmdir`、`fs_unlink`、`fs_rename`、`fs_statvfs`、`fs_sync` |
| 17〜20 | `fs_attach_dev`、`fs_detach_dev`、`fs_mounts`、`fs_utime` |

## クラス3　カレンダ

`dt_gettime`、`dt_settime`、`dt_gmtime`、`dt_localtime`、`dt_mktime`、`dt_getsystz`、`dt_setsystz`(1〜7)。

## クラス5　ソケット

| 番号 | システムコール |
| --- | --- |
| 1〜7 | `so_socket`、`so_close`、`so_bind`、`so_connect`、`so_listen`、`so_accept`、`so_shutdown` |
| 8〜11 | `so_send`、`so_recv`、`so_sendto`、`so_recvfrom` |
| 12〜17 | `so_getsockopt`、`so_setsockopt`、`so_getsockname`、`so_getpeername`、`so_poll`、`so_fcntl` |
| 18〜22 | `so_resolve`、`so_getifaddr`、`so_getdns`、`so_setifaddr`、`so_dhcp_start` |
| 23 | `so_getobj`(ソケットの実身のUUID) |

## クラス6　コンソール

`tm_putstring`、`tm_putchar`、`tm_log_read`(1〜3)。`tm_log_read`は起動時からのコンソール出力を読み出す。

## クラス7　実身

| 番号 | システムコール |
| --- | --- |
| 1〜8 | `ob_opn_obj`、`ob_cls_obj`、`ob_cre_obj`、`ob_del_obj`、`ob_ref_obj`、`ob_lst_obj`、`ob_lnk_obj`、`ob_unl_obj` |
| 9〜14 | `ob_rea_rec`、`ob_wri_rec`、`ob_apd_rec`、`ob_trn_rec`、`ob_del_rec`、`ob_lst_rec` |
| 15〜23 | `ob_get_atr`、`ob_set_atr`、`ob_get_prt`、`ob_set_prt`、`ob_dup_key`、`ob_fnd_nam`、`ob_get_crd`、`ob_login`、`ob_set_pwd` |
| 24〜29 | `ob_att_vol`、`ob_det_vol`、`ob_rea_res`、`ob_wri_res`、`ob_del_res`、`ob_lst_res` |
| 30〜34 | `ob_beg_trx`、`ob_end_trx`、`ob_sch_rec`、`ob_trs_rec`、`ob_cpy_obj` |
| 35〜38 | `ob_ntf_evt`、`ob_can_evt`、`ob_map_rec`、`ob_unm_rec` |
| 39〜43 | `ob_get_dom`、`ob_set_dom`、`ob_get_ico`、`ob_set_ico`、`ob_ref_vol` |
| 44、45 | `ob_fnd_lnk`、`ob_lst_lnk`(箱のリンク先を名前で探す、一覧) |

## クラス8　描画

`dp_fill_rect`、`dp_frame_rect`、`dp_line`、`dp_put_argb`、`dp_text`、`dp_text_width`、`dp_draw_tad`、`dp_img_decode`(1〜8)。自分のウインドウの描画環境にだけ描画できる(10.2)。

## クラス9　ウインドウマネージャ

| 番号 | システムコール | 機能 |
| --- | --- | --- |
| 1、2 | `wm_obj_gid`、`wm_obj_flush` | ウインドウの描画環境を取得する、描画内容を画面に反映する |
| 3 | `wm_key_char` | キーイベントを文字に変換する |
| 4〜9 | `mn_cre_men`、`mn_del_men`、`mn_chg_atr`、`mn_set_lst`、`mn_chg_idx`、`mn_pop_men` | メニュー |
| 10〜15 | `kc_open`、`kc_close`、`kc_hid_key`、`kc_choose`、`kc_list`、`kc_mode` | かな漢字変換 |
| 16 | `wm_msg_put` | メッセージ行 |
| 17、18 | `wm_look`、`wm_set_look` | 外観設定テーブルを読み出す、書き換える(管理者) |
| 19 | `wm_draw_bar` | 自分のウインドウにスクロールバーを描く |
