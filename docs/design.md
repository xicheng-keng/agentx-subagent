# AgentXサブエージェント + 永続化/キャッシュ層 設計方針

## 1. 全体アーキテクチャ

```
[SNMP Manager] --v1/v2c/v3--> [snmpd (master agent)] --AgentX(unix socket)--> [C subagent]
                                                                                   |
                                                                    +--------------+--------------+
                                                                    |                             |
                                                            config.lmdb                      cache.lmdb
                                                        (永続・sync有効)              (揮発・MDB_NOSYNC)
                                                                    |                             |
                                                                    +----------[Rustアプリ]--------+
```

- SNMPv3の認証・暗号化(USM)は **snmpd(マスターエージェント)側**で処理する。C subagentはAgentXプロトコル(RFC2741)でローカルにマスターと接続するのみで、v3スタックを自前実装する必要はない。
- Set/Trapが必須要件のため、サブエージェント本体は **C + net-snmpネイティブAPI** で実装する（Go/Python/Rustの既存AgentXライブラリはSet/Trap未実装または実績不足のため今回は不採用）。
- 対象ハードはRaspberry Pi 5 + SDカード想定（ハード選定は制御不可）。高頻度SNMP GETが想定されるため、ストレージ層はLMDB一本化を採用（詳細は2章）。

## 2. データストア層

### 2.1 LMDB一本化（SQLiteは不採用に変更）
高頻度GETが想定される点を踏まえ、**永続・揮発の両方をLMDBで実装し、SQLiteは使わない**方針に変更した。

理由:
- LMDBはmmap経由の直接メモリアクセスで読み取りを行うため、SQLiteのSQLパース＋B-treeトラバースのオーバーヘッドがなく、GET1回あたりのレイテンシが低い
- リーダー同士・リーダーとライターが互いにブロックしないMVCC設計のため、高頻度GETの最中でも稀な書き込みが割り込んでも応答が止まらない
- OSページキャッシュ層でmmapページがプロセス間共有されるため、Cサブエージェント・Rustアプリ間でキャッシュが二重にならない
- Copy-on-WriteのB+木のため、書き込み中の電源断でも旧世代データは上書きされず残る（SDカードのようなPower-Loss Protectionのないストレージでも構造的に有利）
- 依存ストレージエンジンが1つになり、検証項目・障害モードの理解コストが下がる

### 2.2 環境分離: config.lmdb（永続）/ cache.lmdb（揮発・tmpfs配置）
同一ライブラリ・同一APIで、用途ごとに **LMDB環境（mmapファイル）を2つ**用意する。

- **`config.lmdb`**（永続・設定系）: 物理フラッシュ上の永続パーティションに配置。`MDB_NOSYNC`は使わずfsyncを効かせる。書き込み主体はCサブエージェントのみ（2.4節参照）
- **`cache.lmdb`**（揮発・テレメトリ系）: **tmpfs上に配置**。`MDB_NOSYNC`で高速化。書き込み主体はRustアプリ（2.4節参照）

**tmpfs配置の理由**: 揮発系MIBオブジェクトは装置起動直後は値が未確定で、本来デフォルト値に戻っているべきだが、`cache.lmdb`を物理フラッシュ上に置くと前回終了時点の値が再起動後も残ってしまう。tmpfs（RAM上のファイルシステム）に配置すれば、電源断・再起動のたびに中身が自動的に消え、常に空の状態から起動できる。副次的に、テレメトリ系の頻繁な書き込みが物理SDカードを一切摩耗させなくなる利点もある。

事前に最大マップサイズ（`mdb_env_set_mapsize`）を見積もっておく必要がある。config項目数は概ね既知のため問題になりにくい。

- バインディング: Rust側は `heed`（LMDBの型付きラッパー、Meilisearch製）。C側はLMDB本体のCヘッダを直接使用。
- デバッグ: `sqlite3` CLIほどの知名度はないが、`mdb_dump`/`mdb_stat`で内容確認・統計取得が可能。

### 2.3 起動時初期化（cache.lmdbのデフォルト値ブートストラップ）
tmpfsは起動のたびに空になるため、`cache.lmdb`側だけ追加の初期化ロジックが必要になる。

- Cサブエージェント起動シーケンスで、`cache.lmdb` env作成直後に、揮発系MIBオブジェクトのデフォルト値を一括投入するブートストラップ関数を実行する
- デフォルト値のリストはMIBオブジェクトのメタ情報（MIB定義のDEFVAL等）から、mib2cテンプレート側で自動生成できるようにしておく（3章参照）
- **起動順序の制約**: 「tmpfsマウント → cache.lmdb env作成・デフォルト値投入 → Cサブエージェント/Rustアプリのサービス起動」という順序をsystemdのユニット依存関係（`RequiresMountsFor=`、`Before=`/`After=`）で明示的に縛る必要がある。順序が崩れると、いずれかのプロセスが未初期化の`cache.lmdb`を先に開く可能性がある

### 2.4 Redis不採用の理由（変更なし）
Redisは別プロセス常駐・ネットワーク/ソケット越しの通信コスト・永続化やメモリ上限の運用管理が発生するため、pub/subやTTLなどRedis固有機能が明確に必要でない限り不採用。

### 2.5 書き込み主体（推奨: データ域ごとの単一ライター原則）
「CサブエージェントかRustアプリかどちらかに一律で寄せる」のではなく、**書き込みのトリガー元がどちらかで機能的に決まる**ため、以下のデータ域単位の単一ライター原則を推奨する。

- **SNMP Setで変更される値（＝MIBでwritableな全オブジェクト）**: 書き込みの唯一の正当な起点はSNMP Setであり、それを直接受けるのはCサブエージェントである。したがって **Cサブエージェントを唯一のライター** とする（`config.lmdb`）。Rustアプリがこれらの値を変更したい場合は、`config.lmdb`へ直接書き込まず、Cサブエージェントに対するローカルIPC（4章参照）経由でリクエストし、Cサブエージェント側で書き込みを実行する。
- **Rustアプリ側が収集・生成する監視/テレメトリ系の揮発データ**: **Rustアプリを唯一のライター** とする（`cache.lmdb`）。Cサブエージェントはこれらを読み取り専用（SNMP Getへの応答用）として扱う。
- **書き込み可能だが揮発モード（コンパイルスイッチでvolatile指定）のMIBオブジェクト**: SetのトリガーはSNMPなので、このケースに限りCサブエージェントが`cache.lmdb`への書き込み主体になる（該当キーのみ例外）。

この原則により「`config.lmdb`はCサブエージェントのみが書く」「`cache.lmdb`は基本Rustアプリが書くが、Set経由の揮発オブジェクトだけCサブエージェントが書く」というキー単位で書き手が一意に定まる形になる。

## 3. mib2cカスタマイズ方針

標準の `mib2c.*.conf` はC変数への直結が前提でDBアクセスパターンは組み込まれていない。そのため以下の自作テンプレートを用意する。

### 3.1 振り分け方式: MIBオブジェクト単位のコンパイルスイッチ
- **read-write属性のオブジェクト**: 永続(`config.lmdb`)・揮発(`cache.lmdb`)の両方の実装を生成し、**MIBオブジェクト単位のコンパイルスイッチで切り替え可能**にする。
- **read-only属性のオブジェクト**: 揮発(`cache.lmdb`)のみを生成する（永続実装は不要）。

コンパイルスイッチは、生成コードとは別にオブジェクト名単位で定義するヘッダ（例: `storage_mode.h`）で管理する。

```c
/* storage_mode.h — MIBオブジェクト単位のストレージモード定義 */
#define STORAGE_MODE_PERSISTENT 1
#define STORAGE_MODE_VOLATILE   0

#define STORAGE_MODE_ifAdminStatusExt  STORAGE_MODE_PERSISTENT
#define STORAGE_MODE_tempThreshold     STORAGE_MODE_VOLATILE
```

mib2cテンプレート（`mib2c.dbscalar.conf` 仮称）は、ノードのaccess属性（`$node.access`）を見て生成内容を分岐する。両分岐とも同じLMDB APIを使い、参照するenvハンドル（`config_env`/`cache_env`）だけが異なる。

```c
/* read-write オブジェクトの場合: 両実装 + コンパイルスイッチ */
#if STORAGE_MODE_$node.name == STORAGE_MODE_PERSISTENT
    val = storage_get_int(config_env, "$node.name");   /* config.lmdb */
#else
    val = storage_get_int(cache_env, "$node.name");    /* cache.lmdb */
#endif

/* read-only オブジェクトの場合: cache.lmdb固定、スイッチなし */
val = storage_get_int(cache_env, "$node.name");
```

- 生成コードが呼び出す先として、C側に薄い抽象層 `storage_lmdb.c/.h` を1つ用意する（env分の差異は引数のみ）。SQLite用の別レイヤーは不要になった。

## 4. Rustアプリ⇔Cサブエージェント間IPC

**推奨: Unixドメインソケット（transport）+ Protocol Buffers（メッセージ形式）を、フルのgRPCフレームワークは使わず組み合わせる方式。**

- 理由: gRPCは内部的にHTTP/2スタックを要求し、C側で使うには `grpc-c` 等の重量級ライブラリを追加導入する必要がある。すでにnet-snmp/sqlite3/lmdbへの依存があるCサブエージェントに、HTTP/2 + protobufランタイムをフルで積むのは、組込み・産業監視向けのプロダクトとしては依存関係・ビルド時間・バイナリサイズの面で割に合わない。
- 一方でメッセージ形式だけはProtocol Buffersを採用し、スキーマ駆動でC/Rust両方の型定義・シリアライズコードを自動生成する。C側は `nanopb`（組込み向け軽量protobuf実装）、Rust側は `prost` を使う。これにより「独自バイナリプロトコルを手書きしてメンテする」よりも型安全性とスキーマ管理の面で有利になる。
- トランスポートは素のUnixドメインソケット（`SOCK_STREAM`）上に、4バイト長プレフィックス + protobufシリアライズ済みバイト列、という最小限のフレーミングのみを実装する（HTTP/2やストリーミング制御は不要）。
- メッセージ種別は最小限（例: `WriteConfigRequest{oid, value}` / `WriteConfigResponse{status}`）から始め、必要に応じて拡張する。

## 5. コーディングエージェントへの検証依頼内容

### 5.1 検証対象
1. mib2cカスタムconf（`mib2c.dbscalar.conf` 等）とそこから生成されるCコードの妥当性
2. Rustデモアプリ（heed使用）を用いた、Cサブエージェントとの複数プロセス間競合対策の検証
   - `config.lmdb`: Cサブエージェント単一ライターでの高頻度Set/Get混在時の挙動、電源断疑似テスト時のデータ整合性
   - `cache.lmdb`: 複数プロセスからの同時書き込み試行時のブロッキング/エラー、リーダーの非ブロッキング読み取り、高頻度GET時のレイテンシ
3. AgentX経由のSNMP Setが`config.lmdb`/`cache.lmdb`への書き込みとして正しく反映されること、Trapが正しく発報されること（v3経由でのアクセスはsnmpd側の責務のため、subagentから見た疎通確認のみでよい）
4. Rustアプリ→Cサブエージェント間のUnixドメインソケット+Protocol Buffers IPCが、config書き込み要求を正しく仲介できること（不正メッセージ・切断時の挙動含む）
5. 高頻度SNMP GET負荷（例: 大規模walk相当のリクエスト連打）に対するレイテンシ・スループットの計測（LMDB採用の妥当性確認）
6. `dm-flakey`等によるループデバイス上の擬似電源断/書き込み中断テスト（`config.lmdb`のCopy-on-Write特性による耐障害性の検証）

### 5.2 Docker構成（マルチステージビルド）
- **build stage**: net-snmp/lmdb開発ヘッダ + Rust toolchainを含むビルド環境。Cサブエージェント・Rustデモアプリの両方をここでビルド。
- **test stage**: buildステージの成果物を使い、複数プロセス（Cサブエージェント + Rustデモアプリ + snmpd）を同一コンテナ内（または docker-compose での複数コンテナ）で起動し、競合シナリオを実行。
- **deploy(runtime) stage**: 最小限のランタイム依存のみを含む本番相当イメージ。ビルド成果物のみをCOPYし、ビルドツールチェーンは含めない。

### 5.3 テストシナリオ（最低限）
- `config.lmdb`: Cサブエージェント（単一ライター）への同時Set連打 → 全件データ欠損なく反映されること、デッドロックしないこと。高頻度GETと同時実行してもレイテンシが劣化しないこと
- `cache.lmdb`: 複数プロセスからの同時書き込み → 片方が待機/リトライされ、データ破損が起きないこと。読み取りはライターをブロックしないこと
- サブエージェント再起動時: snmpdとの再接続、登録OIDの復元、`config.lmdb`/`cache.lmdb`のデータ欠損がないこと
- SNMP Set → ストレージ反映 → 別プロセス(Rustアプリ)からの読み取り一貫性
- Rustアプリ→Cサブエージェント間IPC: 同時多重リクエスト時の直列化、ソケット切断・再接続時の挙動
- 擬似電源断（`dm-flakey`）中の書き込み中断 → 再起動後に`config.lmdb`が破損せず、直前の正常な状態に復旧できること

## 6. 前提・確定事項サマリ
- デプロイは同一ホスト前提（複数ホスト分散は現時点でスコープ外）。将来分散が必要になった場合はストレージ層の置き換え等、設計の見直しが必要になる点のみ留意。
- Rust→C間IPCはUnixドメインソケット + Protocol Buffers（nanopb/prost）、gRPCフレームワークは不採用。
- 永続化・揮発ともにLMDBへ一本化（SQLiteは不採用）。高頻度GETに対するレイテンシ・プロセス間キャッシュ共有・Copy-on-Writeによる耐障害性を優先した判断。
- 対象ハードはRaspberry Pi 5 + SDカード想定、ハード選定はスコープ外。ソフトウェア側（LMDBのCoW特性、`config.lmdb`単一ライター化）で耐障害性を担保する方針。
