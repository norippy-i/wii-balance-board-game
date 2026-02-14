#include <M5Unified.h>
#include <Wiimote.h>
#include <stdarg.h>

Wiimote wiimote;

// --- 設定値 ---
const float SQUAT_RATIO = 0.12;    // スクワット判定の閾値(体重比率 12%)
const float DIR_THRESHOLD = 0.15;   // 重心移動の感度(0.0~1.0)

// --- 変数 ---
float baseWeight = 0; // 基準体重
float balX_offset = 0; // 重心Xオフセット
float balY_offset = 0; // 重心Yオフセット
bool uartEnabled = true; // UART送信有効フラグ
bool upDownEnabled = true; // 上下判定有効フラグ

// 前回の送信状態（重複送信防止用）
String lastCmd = "";

// 接続状態管理
enum ConnectionState {
  STATE_DISCONNECTED,
  STATE_SCANNING,
  STATE_CONNECTED,
  STATE_TIMEOUT
};
volatile ConnectionState connState = STATE_DISCONNECTED;
ConnectionState lastConnState = STATE_DISCONNECTED;
unsigned long connectStartTime = 0;

// 表示用バッファ変数（描画と通信を分離するため）
float display_total = 0;
float display_balX = 0;
float display_balY = 0;
String display_cmd = "";
volatile unsigned long lastDataTime = 0; // 最後にデータを受信した時刻
unsigned long lastDisplayCmdTime = 0;    // 表示用コマンドの更新時刻

// グローバル変数（コールバックからのデータ受け渡し用）
volatile float g_tr = 0, g_br = 0, g_tl = 0, g_bl = 0;
volatile float g_total = 0;
volatile bool g_dataUpdated = false;

// プロトタイプ宣言を追加
void wiimote_callback(wiimote_event_type_t event, uint16_t handle, uint8_t *data, size_t len);

void setup() {
  auto cfg = M5.config();
  // UART通信速度設定（PC側の設定と合わせる）
  cfg.serial_baudrate = 115200; 
  M5.begin(cfg);
  M5.update(); // ボタン状態を更新

  // 裏面のGPIO 16(RX), 17(TX)を使ったUART通信を開始 (XIAOとの通信用)
  // M5Stack Coreのデフォルトピン配置: RX2=16, TX2=17
  Serial2.begin(115200, SERIAL_8N1, 16, 17);

  // コールバック関数を登録して初期化
  wiimote.init(wiimote_callback);

  M5.Display.setTextSize(2);
  M5.Display.println("Wii Balance Board");
}

void loop() {
  M5.update();
  
  // 通信処理を最優先（パケット取りこぼし防止のため多めに回す）
  for(int i=0; i<10; i++) wiimote.handle();

  // --- 状態変化時の画面更新 ---
  if (connState != lastConnState) {
    M5.Display.fillRect(0, 130, 320, 30, TFT_BLACK); // ステータスエリア消去
    M5.Display.setCursor(10, 130);
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);

    switch (connState) {
      case STATE_DISCONNECTED:
        M5.Display.print("Disconnected");
        break;
      case STATE_SCANNING:
        M5.Display.print("Scanning... Press SYNC");
        break;
      case STATE_CONNECTED:
        M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
        M5.Display.print("Connected!");
        lastDataTime = millis(); // 接続直後のタイムアウト防止
        break;
      case STATE_TIMEOUT:
        M5.Display.setTextColor(TFT_RED, TFT_BLACK);
        M5.Display.print("Timeout! Rescanning...");
        break;
    }
    lastConnState = connState;
  }

  // --- タイムアウト監視 ---
  // 1. 接続済みだがデータが来ない場合
  if (connState == STATE_CONNECTED && (millis() - lastDataTime > 3000)) {
    connState = STATE_TIMEOUT;
    Serial.println("Data timeout. Force rescan.");
    wiimote.scan(true);
  }

  // キャリブレーション（ボタンAで基準体重リセット）
  if (M5.BtnA.wasPressed()) {
    // 制限を撤廃し、現在の値を基準とする
    baseWeight = g_total;
    
    // 重心オフセットも記録（ゼロ除算防止のため微小荷重以上で計算）
    if (g_total > 1.0) {
      balX_offset = ((g_tr + g_br) - (g_tl + g_bl)) / g_total;
      balY_offset = ((g_tr + g_tl) - (g_br + g_bl)) / g_total;
    } else {
      balX_offset = 0; balY_offset = 0;
    }
    M5.Display.fillScreen(TFT_BLACK);
    M5.Display.setCursor(0, 0);
    M5.Display.println("Manual Calibrated!");
  }

  // UART送信切り替え（ボタンB）
  if (M5.BtnB.wasPressed()) {
    uartEnabled = !uartEnabled;
  }

  // 上下判定切り替え（ボタンC）
  if (M5.BtnC.wasPressed()) {
    upDownEnabled = !upDownEnabled;
  }

  // 描画更新レートの制限 (30FPS程度に抑えて通信処理を優先する)
  static unsigned long lastDrawTime = 0; 
  bool shouldDraw = (millis() - lastDrawTime > 100); // 10FPSに落として負荷軽減

  if (g_dataUpdated) {
    g_dataUpdated = false;

    // --- 1. データ取得 (コールバックで更新された値を使用) ---
    float tr = g_tr;
    float br = g_br;
    float tl = g_tl;
    float bl = g_bl;
    float total = g_total;

    float balX = 0;
    float balY = 0;
    String currentCmd = ""; // 今回送るべきコマンド

    // --- 2. 判定ロジック ---
    // 乗っていない時 (閾値を3kgに下げて手押しテスト可能に)
    if (total < 3.0) {
      baseWeight = 0;
      // コマンドは空文字（リリース）のまま
    } 
    // 乗っている時 (判定ロジック)
    else {
      // 基準体重の更新ロジック (乗り込み完了を検知する)
      static unsigned long heavyTime = 0;
      
      if (baseWeight == 0 && total > 10.0) {
        baseWeight = total;
      } 
      // 現在の体重が基準より重い状態が0.5秒続いたら、基準を更新する
      // (片足->両足への変化や、体重移動による安定値の変化に追従)
      else if (total > baseWeight + 2.0) {
        if (heavyTime == 0) heavyTime = millis();
        else if (millis() - heavyTime > 500) {
            baseWeight = total;
            heavyTime = 0;
        }
      } else {
        heavyTime = 0;
      }

      // 重心バランスの計算 (-1.0 ~ 1.0)
      // X軸: 右(+) / 左(-)  (オフセットを考慮)
      balX = (((tr + br) - (tl + bl)) / total) - balX_offset; 
      // Y軸: 前(+) / 後(-)  (オフセットを考慮)
      balY = (((tr + tl) - (br + bl)) / total) - balY_offset; 

      // --- アルゴリズム実装 ---
      
      // 1. スクワット判定 (抜重検知)
      static unsigned long lastSquatTime = 0; // 前回のスクワット検知時刻

      // 基準体重より一定以上重くなったら「立ち上がり動作」とみなす（加重検知）
      // クールダウン(500ms)を設けて、誤検知を防ぐ
      // 体重の一定比率(SQUAT_RATIO)分重くなったら検知
      if (total > (baseWeight * (1.0 + SQUAT_RATIO))) {
        if (millis() - lastSquatTime > 500) {
            currentCmd = "CMD:SPACE";
            lastSquatTime = millis();
        }
      } 
      // 2. 重心移動判定 (スクワットしていない時のみ)
      else {
        // スクワット検知直後(600ms)は判定を行わない（立ち上がり時のふらつき/Up誤検知防止）
        if (millis() - lastSquatTime > 600) {
            // 各方向の閾値判定
            bool up    = (balY > DIR_THRESHOLD);
            bool down  = (balY < -DIR_THRESHOLD);
            
            // 上下判定が無効な場合は強制的にfalseにする
            if (!upDownEnabled) {
                up = false;
                down = false;
            }

            bool right = (balX > DIR_THRESHOLD);
            bool left  = (balX < -DIR_THRESHOLD);

            // 判定候補
            String nextCmd = "";

            // 8方向判定 (斜め優先)
            if (up && right)      nextCmd = "CMD:UP_RIGHT";
            else if (up && left)  nextCmd = "CMD:UP_LEFT";
            else if (down && right) nextCmd = "CMD:DOWN_RIGHT";
            else if (down && left)  nextCmd = "CMD:DOWN_LEFT";
            else if (up)    nextCmd = "CMD:UP";
            else if (down)  nextCmd = "CMD:DOWN";
            else if (right) nextCmd = "CMD:RIGHT";
            else if (left)  nextCmd = "CMD:LEFT";

            // デバウンス処理（チャタリング防止）
            // リアルタイム性は少し落ちるが、指定時間(150ms)以上継続した場合のみコマンドとして採用する
            static String pendingCmd = "";
            static unsigned long pendingTime = 0;

            if (nextCmd != "") {
                if (nextCmd == pendingCmd) {
                    if (millis() - pendingTime > 150) { // 150ms継続で確定
                        currentCmd = nextCmd;
                    }
                } else {
                    pendingCmd = nextCmd;
                    pendingTime = millis();
                }
            } else {
                pendingCmd = "";
                // 入力なし(リリース)は即時反映
            }
        }
      }
    }

    // --- 3. UART送信 (状態が変わった時だけ送る) ---
    // UARTが無効な場合は強制的にリリース状態(空文字)として扱う
    // これによりOFFになった瞬間にRELEASEが送信され、以降は送信されなくなる
    String targetCmd = uartEnabled ? currentCmd : "";

    if (targetCmd != lastCmd) {
      // コマンドが切り替わる時は必ずRELEASEを送って前のキーを離す
      // これにより「右」から「右上」への変更などもスムーズに行える
      if (lastCmd != "") {
        Serial2.println("CMD:RELEASE");
      }
      
      if (targetCmd != "") {
        Serial2.println(targetCmd);
      }
      
      lastCmd = targetCmd;
    }
    
    // 表示用変数へコピー
    display_total = total;
    display_balX = balX;
    display_balY = balY;
    
    // 表示用コマンドは、新しいコマンドが来た時だけ更新し、少しの間維持する
    if (currentCmd != "") {
        display_cmd = currentCmd;
        lastDisplayCmdTime = millis();
    } else if (millis() - lastDisplayCmdTime > 500) {
        display_cmd = ""; // 0.5秒経過したら消す
    }
  }
  
  // --- 4. 情報表示 (データ受信とは非同期で定期更新) ---
  if (shouldDraw) {
    lastDrawTime = millis();

    if (connState == STATE_CONNECTED) {
       // データ正常受信中
       M5.Display.setCursor(0, 40);
       M5.Display.setTextSize(3);
       
       if (display_cmd != "") {
           M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
           M5.Display.printf("CMD: %-12s\n", display_cmd.substring(4).c_str());
       } else {
           M5.Display.setTextColor(TFT_DARKGRAY, TFT_BLACK);
           M5.Display.printf("CMD: %-12s\n", "---     ");
       }

       M5.Display.setTextColor(TFT_WHITE, TFT_BLACK);
       // 体重表示は削除
       // M5.Display.printf("Kg : %5.1f   \n", display_total);
       
       M5.Display.println(); // スペースを追加

       M5.Display.setTextSize(2);
       M5.Display.printf("X:%5.2f Y:%5.2f  \n", display_balX, display_balY);

       // UART状態表示
       M5.Display.setCursor(10, 180);
       if (uartEnabled) {
           M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
           M5.Display.print("UART: ON ");
       } else {
           M5.Display.setTextColor(TFT_RED, TFT_BLACK);
           M5.Display.print("UART: OFF");
       }

       // 上下判定状態表示
       M5.Display.setCursor(170, 180);
       if (upDownEnabled) {
           M5.Display.setTextColor(TFT_GREEN, TFT_BLACK);
           M5.Display.print("UD: ON ");
       } else {
           M5.Display.setTextColor(TFT_RED, TFT_BLACK);
           M5.Display.print("UD: OFF");
       }

       // 操作ガイドを表示
       M5.Display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
       M5.Display.setCursor(10, 200);
       M5.Display.print("BtnA:Calib BtnC:UD");
       M5.Display.setCursor(10, 220);
       M5.Display.print("BtnB: UART On/Off");
    }
  }
}


// Wiimoteライブラリのコールバック関数 (ここでは重い処理を絶対にしないこと！)
void wiimote_callback(wiimote_event_type_t event, uint16_t handle, uint8_t *data, size_t len) {
  if (event == WIIMOTE_EVENT_DATA) {
    // どのようなデータでも受信したらタイムアウトをリセット
    lastDataTime = millis();
    
    // ライブラリのキャリブレーション済み重量取得関数を使用
    float weights[4];
    wiimote.get_balance_weight(data, weights);

    // 取得した正確な重量(kg)をグローバル変数へ
    g_tr = weights[BALANCE_POSITION_TOP_RIGHT];
    g_br = weights[BALANCE_POSITION_BOTTOM_RIGHT];
    g_tl = weights[BALANCE_POSITION_TOP_LEFT];
    g_bl = weights[BALANCE_POSITION_BOTTOM_LEFT];
    
    g_total = g_tr + g_br + g_tl + g_bl;
    g_dataUpdated = true;
    
  } else if (event == WIIMOTE_EVENT_CONNECT) {
    connState = STATE_CONNECTED;
    wiimote.set_led(handle, 1); // LEDを点灯させて接続完了を通知
  } else if (event == WIIMOTE_EVENT_DISCONNECT) {
    connState = STATE_DISCONNECTED;
    wiimote.scan(true); // 切断されたら再スキャン
  } else if (event == WIIMOTE_EVENT_INITIALIZE) {
    connState = STATE_SCANNING;
    wiimote.scan(true); // 常にスキャン開始
  }
}