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
| 78 | `tk_chg_slt`(T-Kernel 2.0。タイムスライス、4.3.1) |
| 79、80 | `tk_set_asp`、`tk_ref_asp`(TessronOSの拡張。割付けCPUを呼び手が大きさを渡すビットの配列で設定、参照する。4.3.2) |

装置の割込みを受けるCPUを決める`SetIntTarget`、`GetIntTarget`(TessronOSの拡張、4.5.1)は、ドライバのためのカーネルの中の関数で、SVCは無い。プロセスは装置の実身の属性`irq.cpu`(クラス7の属性の書込み、8.4)と、機器の設定の実身の`CPU_CHORE`、`CPU_RT`の行で決める。

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
| 16 | `ts_ref_cpu` | 動いているCPUの数と、各CPUがタスクを走らせた時間。呼び手が大きさを渡す(`ts_ref_cpu(pk, size)`、4.3) |
| 17、18 | `ts_def_flt`、`ts_flt_ret` | プログラムが受け取るデータアボートのエントリを決める、例外の時点の状態(書き換えたもの)へ戻る(5.5.2) |
| 19 | `ts_spn_prc` | POSIXのプロセスを、パス(objfsの木)で指したプログラムの実身から起動する(5.8) |
| 20〜34 | `ts_get_inh`、`ts_wai_psx`、`ts_kil_prc`、`ts_sig_set`、`ts_sig_ret`、`ts_sig_sus`、`ts_slp_psx`、`ts_sig_alm`、`ts_set_pgid`、`ts_get_pgid`、`ts_set_sid`、`ts_get_sid`、`ts_get_ppid`、`ts_ref_psx`、`ts_lst_prc` | POSIXのプロセスの記述子、待ち、シグナル、プロセスグループ、セッション(`include/ts/psx.h`) |
| 35 | `ts_spn_obj` | POSIXのプロセスを、仮身の指すプログラムの実身(UUID)から起動する(5.8) |
| 36 | `ts_lkp_obj` | 実身の木(objfs、「/」から)のパスが指す実身のUUIDを、呼んだプロセスの資格で引く(5.8) |

## クラス2　ファイル(FAT)

| 番号 | システムコール |
| --- | --- |
| 1〜10 | `fs_open`、`fs_close`、`fs_read`、`fs_write`、`fs_lseek`、`fs_stat`、`fs_fstat`、`fs_ftruncate`、`fs_truncate`、`fs_getdents` |
| 11〜16 | `fs_mkdir`、`fs_rmdir`、`fs_unlink`、`fs_rename`、`fs_statvfs`、`fs_sync` |
| 17〜20 | `fs_attach_dev`、`fs_detach_dev`、`fs_mounts`、`fs_utime` |
| 21〜29 | `fs_dup`、`fs_pipe`、`fs_xstat`、`fs_fxstat`、`fs_ioctl`、`fs_fcntl`、`fs_poll`、`fs_link`、`fs_chmod`(POSIXの層の、5.8) |
| 30〜38 | `fs_opn_vol`、`fs_openat`、`fs_statat`、`fs_mkdirat`、`fs_rmdirat`、`fs_unlinkat`、`fs_utimeat`、`fs_renameat`、`fs_fstatvfs`(ディスクのデバイスの実身の鍵から、パスの木につながずに開くディスクの中、7.2) |
| 39 | `fs_fsync`(1つの記述子のファイルを媒体に置く。POSIXの`fsync`と`fdatasync`、7.3) |

## クラス3　カレンダ

`dt_gettime`、`dt_settime`、`dt_gmtime`、`dt_localtime`、`dt_mktime`、`dt_getsystz`、`dt_setsystz`(1〜7)、`dt_getsyszone`、`dt_setsyszone`(8、9。タイムゾーンのIANAの名前、9.5)、`dt_getrtc`、`dt_setrtc`(10、11。x64だけ。PCのRTCをローカル時刻とUTCのどちらとして扱うか、9.5.1)、`dt_getrtc`、`dt_setrtc`(10、11。x64だけ。PCのRTCの扱い、9.5.1)。

## クラス5　ソケット

| 番号 | システムコール |
| --- | --- |
| 1〜7 | `so_socket`、`so_close`、`so_bind`、`so_connect`、`so_listen`、`so_accept`、`so_shutdown` |
| 8〜11 | `so_send`、`so_recv`、`so_sendto`、`so_recvfrom` |
| 12〜17 | `so_getsockopt`、`so_setsockopt`、`so_getsockname`、`so_getpeername`、`so_poll`、`so_fcntl` |
| 18〜22 | `so_resolve`、`so_getifaddr`、`so_getdns`、`so_setifaddr`、`so_dhcp_start` |
| 23 | `so_getobj`(ソケットの実身のUUID) |
| 24 | `so_resolveall`(名前のIPv4とIPv6のアドレス、9.3.2) |
| 25 | `so_recvtos`(`so_recvfrom`に、データグラムが運んだTOSかトラフィッククラスを加えたもの、9.2) |

IPv6のアドレスの状態(`so_getifaddr6`、`so_getdns6`、`so_getip6`)と設定(`so_setip6`、`so_setdns6`)はSVCにしていない。プロセスは今の状態をシステムの実身のレコード7の`IPV6`、`ADDRESS6`、`GATEWAY6`、`DNS6`の行で読み、設定はネットワーク設定の実身の`$$IPV6`の行を書き換えて変える(9.3.4)。

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
| 46 | `ob_end_adm`(一時的に得た管理者の権限を返す、8.7) |
| 47 | `ob_wip_vol`(実身のあるボリュームのジャーナルを、置き終えてから0で書きつぶす。その実身のレコードを書ける者だけ、7.3) |
| 48 | `ob_get_atv`(メタデータの1つのキーの値) |
| 49 | `ob_rpl_rec`(レコードの中身を丸ごと置き換える。ネイティブのTSFSでは新しい場所に書いてから1回の変更で差し替えるので、途中で電源が切れても前の中身か新しい中身のどちらかが残る。それができない置き場では上書きしてから切り詰める、7.3) |
| 50 | `ob_syn_obj`(その鍵で書いたものを媒体に置く。閉じるときと同じ確定を待つ、7.3) |
| 51 | `ob_rea_tmo`(`ob_rea_rec`を、鍵の開き方によらず、最大で指定のミリ秒だけ待って読む。`TMO_POL`は待たず、`TMO_FEVR`は来るまで待つ。時間切れは`E_TMOUT`。チャネルのように何かが来るものだけが答え、ほかは`E_NOSPT`。プログラムが通知のポートを見に来る代わりに待つのに使う、11.30) |
| 52 | `ob_fnd_sys`(根「システム」の仮身のうち、指定の続柄のものが指す実身。仮身がないか、指す実身がないときは、既定のUUIDの実身。どちらもなければ`E_NOEXS`。呼んだプロセスの資格で根を読む、8.13) |
| 53 | `ob_acc_obj`(呼んだプロセスが実身に指定の操作をすべてできるかを、実身を開かずに答える。Unixの`access(2)`に当たる。`ob_opn_obj`と同じ判定(資格、ボリュームの保護ドメイン、ドロップで得た権限)で、できれば`E_OK`、できなければ`E_OACV`、実身がなければ`E_NOEXS`。断ったことにはならないので`!OACV!`の行は出さない、8.7.4) |
| 54 | `ob_ref_vob`(仮身の見た目を、デスクトップの`om_store`が持つものから最大16個まとめて読む。実身名、仮身の帯の文字、アイコン、紙の色、状態(`TV_ST_GONE`、`TV_ST_OTHER`)。ほかのボリュームにあるかは、渡した実身(描いているウインドウの実身)を基準にする。呼んだプロセスが読めない実身は虚身として答える。アイコンが渡した場所に入らないときは大きさだけを答え、`OB_VW_SHORT`を立てる。表示部をプロセスに結んだアプリケーションが`T_TVSRC`の答えに使う、11.11) |
| 55 | `ob_ref_vid`(実身のあるボリュームのUUIDと、この機械のUUIDを答える。仮身の`id`は、実身が仮身を持つ実身と別のボリュームにあるとき、実身のUUIDの前にそのボリュームのUUIDを書く(8.3)。レコードを自分で書くアプリケーション(基本図形編集、予定表、基本表計算)が、保存の前に仮身にボリュームを書くのに使う。書き換えは`include/ts/tadlnk.h`の`tad_lnk_vol_doc`(解析したレコード)と`tad_lnk_vol_fix`(xmlTADのテキスト)で、デスクトップの保存と同じソースを使う。実身の属性を読む権利が要る。どちらかの受け取り先はNULLでよく、機械のUUIDだけを聞くときは実身もNULLでよい。ボリュームにない実身は`E_NOSPT`) |
| 56 | `ob_dup_obj`(実身を、デスクトップが原紙から新しい実身を作るときと同じく複写する。中身は`om_store_copy`(深くない複写)で、複写は名前を与えたもの(NULLなら元の名前)、日付は今、参照数は0になり、レコードの仮身の行き先は1つずつ多く数える。図形の文字枠の仮身化で、プロセスの基本図形編集が基本文章編集の原紙から新しい実身を作るのに使う。元の実身のレコードと属性を読む権利が要る。名前が空なら`E_PAR`、実身でなければ`E_NOSPT`) |

音の出力先の選択はSVCにしていない。プロセスは、出力先の一覧と選んだ出力先をサウンドデバイスの実身のレコード24と25(`OB_SND_OUTS`、`OB_SND_OUTSEL`)か、システムの実身のレコード8で読み、選ぶときはシステムの実身のレコード8に`SELECT`と出力先のIDを書く(6.5.1)。システムの音(ブザー、クリック音、〈試聴〉の音)を選んだ出力先で鳴らすときも、サウンドデバイスを自分で開かず、同じレコード8に`PLAY`と音の名前を書く。書込みの戻り値で鳴ったかどうかが分かる(6.5.5)。音量もSVCにしていない。ミキサーの全体の音量(左と右)と消音、音のデバイスを使っているプログラムの一覧とそれぞれの音量と消音は、サウンドデバイスの実身のレコード26(`OB_SND_MIX`、`SDMix`)で読み、レコード27(`OB_SND_MIXSET`、`SDMixSet`)に書いて変える(6.5.6)。

## クラス8　描画

`dp_fill_rect`、`dp_frame_rect`、`dp_line`、`dp_put_argb`、`dp_text`、`dp_text_width`、`dp_draw_tad`、`dp_img_decode`(1〜8)。自分のウインドウの描画環境にだけ描画できる(10.2)。

9〜26は、文章と図形の表示部をプロセスに結んだアプリケーション(11.11)が使う。名前と引数はカーネルの同名の関数と同じで、表示部のソースはカーネルでもプロセスでも同じものを使う(`include/ts/tvport.h`)。どれも自分のウインドウの描画環境(`wm_obj_gid`)と自分で開いたメモリの描画環境(22)にだけ働き、ほかは`E_ID`。点の並びと模様(`T_DPPAT`の画素とマスク)はカーネルのメモリに写してから使う。

| 番号 | システムコール | 機能 |
| --- | --- | --- |
| 9〜11 | `dp_set_visible`、`dp_ref`、`dp_set_mode` | 見える所を絞る、描画環境を読む、混ぜ方を変える |
| 12〜17 | `dp_put_pixel`、`dp_fill_poly`、`dp_draw_poly`、`dp_line_wide`、`dp_frame_oval`、`dp_put_image` | 点、多角形、折れ線、太い線、楕円の線、絵を箱の大きさで置く |
| 18 | `dp_scale` | 描画環境の倍率(百分率) |
| 19、20 | `fn_system`、`fn_find_style` | システムの書体、書体名の並びと字形から選んだ書体のID。書体はカーネルのものを使う |
| 21 | `fn_dra_txt` | 字を描く、測る(`T_FNTXT`の`op`で`fn_draw`、`fn_width`、`fn_vdraw`、`fn_vwidth`、`fn_extent`、`fn_fit`、`fn_metrics`、`fn_has_vert`を選ぶ)。書体の大きさ、字形、角度はプロセスが持ち、呼ぶたびに渡す。カーネルは書体の層を保持している間だけ書体をその状態にして、終わると元に戻す。プロセスの`fn_set_size`などは`lib/libtv/tv_fn.c`がプロセスの中で受ける |
| 22、23 | `dp_open_mem`、`dp_close` | メモリの描画環境を開く、閉じる。`T_DPBMP`(画素の場所、行の間のバイト数、幅、高さ)で渡したプロセスの絵を、開いたときにカーネルに写し、9〜21と1〜4の描画で描く。`dp_close`で絵をプロセスに書き戻して閉じる。描けるのは開いたプロセスだけで、プロセスが終わると閉じる(書き戻さない)。`dp_close`が閉じるのは、そのプロセスが開いたメモリの描画環境だけで、ほかは`E_ID`。画素の数が`DP_MEM_MAX`(4096×4096)を超えるか、行の間が幅より短いときは`E_PAR`。同時に開いておけるのは、すべてのプロセスを合わせて8つまで(超えると`E_LIMIT`)。プロセスの基本図形編集の焼き付けが、図形を絵の画素に描くのに使う(17.13) |
| 24〜26 | `fn_nface`、`fn_face_at`、`fn_family` | カーネルが使っている書体の数、その中のi番目(0から)の書体のID(範囲の外は`E_NOEXS`)、書体の登録されたファミリ名(`max`-1バイトまでで切り、終わりに0を置く。`max`は256まで。IDが違うか`max`が0以下なら`E_PAR`、書けない場所なら`E_MACV`)。dpとfnはカーネルに置き、プロセスはSVCで呼ぶ(BTRON3のdpと同じ形)。プロセスの基本文章編集が書体の一覧を作るのに使う(11.3) |

## クラス9　ウインドウマネージャ

| 番号 | システムコール | 機能 |
| --- | --- | --- |
| 1、2 | `wm_obj_gid`、`wm_obj_flush` | ウインドウの描画環境を取得する、描画内容を画面に反映する |
| 3 | `wm_key_char` | キーイベントを文字に変換する |
| 4〜9 | `mn_cre_men`、`mn_del_men`、`mn_chg_atr`、`mn_set_lst`、`mn_chg_idx`、`mn_pop_men` | メニュー |
| 10〜15 | `kc_open`、`kc_close`、`kc_hid_key`、`kc_choose`、`kc_list`、`kc_mode` | かな漢字変換 |
| 16 | `wm_msg_put` | メッセージ行 |
| 17、18 | `wm_look`、`wm_set_look` | 外観設定テーブルを読み出す、書き換える。ユーザ環境設定の項目はユーザ情報の実身に、ウインドウの見た目の項目は機器の設定の実身に書き込めるプロセスが、それ以外は管理者だけが書き換えられる。ユーザ環境設定とウインドウの見た目の項目は範囲を確かめ、ウインドウの見た目はすぐに画面へ反映する |
| 19 | `wm_draw_bar` | 自分のウインドウにスクロールバーを描く |
| 20〜29 | `tr_psh_dat`、`tr_add_dat`、`tr_set_dat`、`tr_get_sts`、`tr_ref_rec`、`tr_rea_rec`、`tr_sel_dat`、`tr_mov_dat`、`tr_del_dat`、`tr_clr_tra` | トレー(10.9)。カーネルのタスクが代わりに行い、バイト列はカーネルのメモリを通して受け渡す |
| 30、31 | `wm_obj_surf`、`wm_obj_put` | 作業領域の面をプロセスの共有メモリ領域にマッピングする、面に描いた矩形をウインドウに置いて画面に反映する(10.2) |
| 32 | `wm_key_put` | 入力の対象にならないウインドウ(`WM_ATTR_NOFOCUS`)から、入力の対象のウインドウへキーを入れる。そのウインドウが押された直後だけ受け付ける(11.15) |
| 33〜35 | `km_get`、`km_set`、`km_label` | キーの配列と付け替えを読む、設定する(機器の設定を書き込めるプロセス)、キーに書く文字を得る(11.15) |
| 36 | `wm_screen` | 画面の幅と高さ(ブラウザの`screen.width`と`screen.height`など) |
| 37 | `wm_hit_slop` | 押下の時刻を渡し、その押下が指のときにつまみの当たりを広げる画素の数を得る。マウスとペンの押下には0(10.4.6) |
| 38 | `wm_grab_screen` | 画面の矩形を、ポインタの有無を選んでプロセスのメモリに写す。ブラウザが、利用者がパネルで許した画面の共有(`getDisplayMedia`)に使う(10.4.3、14.4.8) |
| 39 | `wm_share_list` | 共有できるウインドウ(名前のバーがあり、表示されていて、従属していないもの)を手前から順に、番号、通し番号、大きさ、名前で返す |
| 40 | `wm_grab_window` | ウインドウの作業領域を、隠れていてもプロセスのメモリに写す。閉じたウインドウは`E_NOEXS`、途中で大きさが変わると`E_BUSY` |
| 41 | `wm_press_area` | 指の押下をすぐ主ボタンの押下にする区画を、作業領域の座標の矩形の一覧で登録する(6.4.1) |
| 42 | `wm_fill_look` | 描ける描画環境の矩形を、外観設定テーブルの項目で、システムが塗るとおり(濃淡や縞、`wm_look_pat`)に塗る。項目がなければ作業領域の色。プロセスのウインドウの地(`LK_PNLGROUND`)をデスクトップのウインドウと同じに塗るのに使う |

## クラス10　GPU

`gp_opn_gpu`、`gp_cls_gpu`、`gp_ref_gpu`、`gp_cre_bo`、`gp_del_bo`、`gp_map_bo`、`gp_sub_job`、`gp_wai_fen`、`gp_wai_bo`、`gp_shr_bo`、`gp_imp_bo`、`gp_ref_bo`(1〜12)、`gp_bnd_bo`(13。x64だけ。プロセスが選んだGPUアドレスにBOを置く、13.11.3)。BOは自分のコンテキストの表の番号でだけ指定できる(13.5)。

## クラス11　シリアルポート

| 番号 | システムコール | 機能 |
| --- | --- | --- |
| 1 | `sr_lst_prt` | シリアルポート(種類が`TDK_SERIAL`のデバイス)の名前、説明、コンソールかどうか、速度の一覧 |
| 2 | `sr_opn_prt` | ポートを名前で開き、プロセスの中の番号を返す |
| 3 | `sr_cls_prt` | 閉じる |
| 4 | `sr_rea_prt` | 届いた分を読む。何も届いていなければ、指定の時間まで最初のバイトを待つ |
| 5 | `sr_wri_prt` | すべてのバイトを送ってから戻る |
| 6 | `sr_set_spd` | 速度(ビット毎秒)を設定する |

ポートはプロセスの番号でだけ指定できる。デバイスはカーネルが開いて持ち、プロセスが終わると閉じる(6.2)。

## クラス12　プログラムのUSBデバイス

| 番号 | システムコール | 機能 |
| --- | --- | --- |
| 1 | `us_lst_dev` | USBデバイスの一覧(`T_USBDEV`。VIDとPID、クラス、速度、ポート、役割、製品名) |
| 2 | `us_get_dsc` | デバイスの記述子を読む |
| 3 | `us_opn_dev` | クラスドライバが取らなかったデバイスを開き、プロセスの中の番号を返す |
| 4 | `us_cls_dev` | 閉じる |
| 5 | `us_ctl` | コントロールエンドポイントへの要求 |
| 6 | `us_set_alt` | インタフェースの代替設定を選ぶ |
| 7 | `us_clr_hlt` | エンドポイントの停止を解く |
| 8 | `us_xfer` | バルク、インタラプト、アイソクロナスのエンドポイントの転送 |
| 9 | `us_rst_dev` | デバイスをリセットし、アドレスと構成を元どおりにする |

開いたデバイスはプロセスの番号でだけ指定でき、プロセスが終わると閉じる(6.3.1)。

## 権限の表

システム全体に効くシステムコールは、次の権限を持つプロセスだけが呼べる。SVCゲート(`svc_gate.c`の`svc_rights`)が呼び出す前に表を引き、権限がなければ`E_OACV`を返す。表は`include/ts/svc.h`の機能コードの定義と並べて持ち、権限の種類は`TSN_RIGHT_*`、各機能コードの行の注記に要る権限を書いてある。管理者(システムのユーザ、または管理者のグループに属するユーザ)はすべての権限を持つ。システムプロセスのタスクは表を引かない。カーネルの中の各層が以前から行っている検査(`knl_dt_may_set`、`ob_att_vol`の管理者の検査など)はそのまま残し、カーネル自身の呼出しにも効く。

| 権限 | 持つもの | システムコール |
| --- | --- | --- |
| `TSN_RIGHT_CLOCK` | 時計の実身(デバイス)に書き込めるもの | `tk_set_tim`、`tk_set_tim_u`、`tk_set_utc`、`dt_settime` |
| `TSN_RIGHT_ZONE` | 暦の実身の属性を読み書きできるもの | `dt_setsystz`、`dt_setsyszone`、`dt_setrtc` |
| `TSN_RIGHT_CONF_DEV` | 機器の設定の実身に書き込めるもの | `km_set` |
| `TSN_RIGHT_CONF_USER` | ユーザ情報の実身に書き込めるもの | `wm_set_look`(項目によってはさらに機器の設定の実身への書込みか管理者であることを求める) |
| `TSN_RIGHT_ADMIN` | 管理者 | `so_setifaddr`、`so_dhcp_start`、`ob_att_vol`、`ob_det_vol`、`ob_set_dom` |

時計の実身の保護は所有者がシステム、グループが管理者、モードが`rw-r--r--`で変えられないので、時刻を変えられるのは実際には管理者だけである。暦、機器の設定、ユーザ情報の実身は、ACLで一般の利用者のグループ(利用者)に書込みを与えてあるので、デスクトップから起動したプログラムはタイムゾーン、キー配列、ユーザ環境設定の項目を変えられる(8.7)。

表にないシステムコールは、呼出し元自身のもの(自分のタスク、ファイル記述子、ソケット、ウインドウなど)か、キー、実身の保護、プロセスの実身が許すものにしか働かない。次のものはシステム全体に効くが、表ではなく実身の保護で守る。

| 操作 | 守るもの |
| --- | --- |
| ディスクの取り付けと取り外し(`fs_attach_dev`、`fs_detach_dev`) | ディスクのデバイスの実身のキーとその権限(8.6。システムが何も使っていないUSBの媒体は一般の利用者も読み書きできる) |
| 電源の切断と再起動 | システムの実身(デバイス)への書込み。所有者がシステム、グループが管理者、モードが`rw-rw-r--`で、ACLで一般の利用者のグループにも書込みを与える(8.6) |
| 音の出力先の選択 | システムの実身のレコード8への書込み(電源と同じ保護)。誰でも読み書きできるサウンドデバイスの実身からは選べない(6.5.1) |
| ネットワーク、機器、ユーザの設定 | 設定の実身の保護(8.9) |
| ほかのプロセスの終了、待ち、メッセージ | プロセスの実身の保護(`knl_obprc_may`、5.5) |

デスクトップから起動したプログラムは管理者でない資格で動く(8.7)。表の権限を持たない呼出しは`E_OACV`で断られ、プログラムはデスクトップに管理者のパスワードを尋ねさせて(`DT_RQ_ADMIN`)、その操作の間だけ管理者の権限を得る。時刻を変えるユーザ環境設定の小物(`application/userenv`)、ネットワーク設定の小物(`application/netenv`)、ネットワークを開始するファイル変換(`application/xfconv`)がこの手順をとる。カーネルのタスクとして動くネットワークの時刻合わせは、システムプロセスのタスクなので表を引かない。キー配列の小物(`application/keymap`)、システム環境設定の小物(`application/sysenv`)の配色、壁紙、画面、キーボードの設定は、一般の利用者が書ける実身への書込みなので尋ねない。
