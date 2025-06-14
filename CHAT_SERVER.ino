#include <WiFi.h>
#include <ESPAsyncWebServer.h>
#include <AsyncTCP.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <map>
#include <vector>
#include <ArduinoJson.h> // For more robust data handling

// --- Display Definitions ---
#define TFT_CS     5
#define TFT_RST    4
#define TFT_DC     2

// --- LED Pin Definition ---
#define LED_PIN    27 // Define the LED pin connected to ESP32 GPIO 27

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

// --- Wi-Fi Configuration ---
const char* ssid = "ChatESP32";
const char* password = "12345678";

// --- Web Server and WebSocket Setup ---
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");

// --- User Management and Chat History ---
struct User {
  String username;
  String password;
};

std::vector<User> users;
std::map<uint32_t, String> loggedInUsers; // Client ID -> username
std::vector<String> chatHistory;
const int MAX_HISTORY = 100; // Max number of chat messages to store

// --- TFT State Management Variables ---
enum TftDisplayState {
  TFT_STATE_IDLE,
  TFT_STATE_NOTIFICATION,
  TFT_STATE_MESSAGE
};

TftDisplayState currentTftState = TFT_STATE_IDLE;
unsigned long tftStateStartTime = 0; // When the current TFT display state began
const unsigned long NOTIFICATION_DURATION = 1500; // 1.5 seconds for notification
const unsigned long MESSAGE_DISPLAY_DURATION = 10000; // 10 seconds for full message

String currentTftMessageFrom = "";
String currentTftMessageContent = "";

// --- LED State Management Variables ---
unsigned long ledFlashStartTime = 0;
const unsigned long LED_FLASH_DURATION = 300; // LED will flash for 300 milliseconds
bool isLedActive = false; // Tracks if the LED is currently meant to be ON for a flash


// --- Function Prototypes ---
void appendChatHistory(String entry);
bool userExists(String username);
bool validateLogin(String username, String pass);
void registerUser(String username, String pass);
void broadcastUserList();
void sendChatHistory(AsyncWebSocketClient *client);
void drawNotificationScreen(String from, String message); // New: Direct drawing function for notification
void drawMessageScreen(String from, String msg);         // New: Direct drawing function for full message
void displayIdleScreen();                               // Function for the idle display state (draws itself)
void setTftStateNotification(String from, String message); // Sets state and draws notification
void setTftStateMessage(String from, String msg);         // Sets state and draws full message
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len);
void updateTftDisplayState(); // Renamed and refined to manage transitions
void handleLedFlash();

// --- Chat History Management ---
void appendChatHistory(String entry) {
  if (chatHistory.size() >= MAX_HISTORY) {
    chatHistory.erase(chatHistory.begin()); // Remove oldest message
  }
  chatHistory.push_back(entry);
}

// --- User Authentication and Registration ---
bool userExists(String username) {
  for (const auto &user : users) {
    if (user.username == username) {
      return true;
    }
  }
  return false;
}

bool validateLogin(String username, String pass) {
  for (const auto &user : users) {
    if (user.username == username && user.password == pass) {
      return true;
    }
  }
  return false;
}

void registerUser(String username, String pass) {
  users.push_back({username, pass});
  Serial.println("Registered new user: " + username);
}

// --- WebSocket Communication Helpers ---
void broadcastUserList() {
  String list = "USERS:";
  for (const auto &pair : loggedInUsers) {
    list += " " + pair.second;
  }
  ws.textAll(list);
  Serial.println("Broadcasting user list: " + list);
  // Trigger an update to the idle screen if currently in idle state
  if (currentTftState == TFT_STATE_IDLE) {
    displayIdleScreen(); // Redraw idle screen with updated user list
  }
}

void sendChatHistory(AsyncWebSocketClient *client) {
  for (const auto &entry : chatHistory) {
    client->text(entry);
  }
}

// --- TFT Direct Drawing Functions ---
void drawNotificationScreen(String from, String message) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(0, 0);
  tft.println("New Msg from:");
  tft.setTextColor(ST77XX_GREEN);
  tft.println(from);
  tft.println("----------------");
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextWrap(true);
  tft.println(message.substring(0, min((int)message.length(), 40)));
}

void drawMessageScreen(String from, String msg) {
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 0);
  tft.setTextWrap(true);
  tft.setTextColor(ST77XX_GREEN);
  tft.setTextSize(1);
  tft.println("FROM: " + from);
  tft.println("----------------");
  tft.setTextColor(ST77XX_CYAN);
  tft.println("MSG: ");
  tft.setTextColor(ST77XX_WHITE);
  tft.println(msg);
}

// --- Function to display the idle screen ---
void displayIdleScreen() {
  tft.fillScreen(ST77XX_BLACK);
  tft.setCursor(0, 0);
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_WHITE);
  tft.println("ESP32 Chat Server");
  tft.println("-----------------");
  tft.setTextColor(ST77XX_CYAN);
  IPAddress IP = WiFi.softAPIP();
  tft.println("AP IP: " + IP.toString());
  tft.println("\nWaiting for msgs..."); // Two newlines for spacing

  // Display online users only if there are any
  if (!loggedInUsers.empty()) {
      tft.setTextColor(ST77XX_YELLOW);
      tft.println("\nOnline Users:"); // One newline before "Online Users:"
      tft.setTextColor(ST77XX_GREEN);
      int y_cursor = tft.getCursorY();
      int count = 0;
      for (const auto &pair : loggedInUsers) {
          if (count < 5) { // Limit to 5 users for display clarity on small screen
              tft.setCursor(0, y_cursor);
              tft.println("- " + pair.second);
              y_cursor += 8; // Assuming text size 1, each line is 8 pixels high
              count++;
          } else {
              break;
          }
      }
  } else {
      tft.setTextColor(ST77XX_RED);
      tft.println("\nNo users online.");
  }
}

// --- Functions to set TFT state and trigger initial draw ---
void setTftStateNotification(String from, String message) {
  if (currentTftState != TFT_STATE_NOTIFICATION) { // Only redraw if changing state
    currentTftState = TFT_STATE_NOTIFICATION;
    tftStateStartTime = millis();
    currentTftMessageFrom = from;
    currentTftMessageContent = message;
    drawNotificationScreen(from, message); // Draw immediately upon state entry
    Serial.println("TFT State: Notification (drawn)");
  }
}

void setTftStateMessage(String from, String msg) {
  if (currentTftState != TFT_STATE_MESSAGE) { // Only redraw if changing state
    currentTftState = TFT_STATE_MESSAGE;
    tftStateStartTime = millis();
    currentTftMessageFrom = from;
    currentTftMessageContent = msg;
    drawMessageScreen(from, msg); // Draw immediately upon state entry
    Serial.println("TFT State: Full Message (drawn)");
  }
}

// --- New: Function to manage TFT display state transitions ---
void updateTftDisplayState() {
  unsigned long currentTime = millis();

  switch (currentTftState) {
    case TFT_STATE_IDLE:
      // No automatic transition out of IDLE.
      // Redraw only happens if explicitly called by broadcastUserList or setup.
      break;

    case TFT_STATE_NOTIFICATION:
      if (currentTime - tftStateStartTime >= NOTIFICATION_DURATION) {
        // Transition to full message display
        setTftStateMessage(currentTftMessageFrom, currentTftMessageContent);
      }
      // No continuous redraw within NOTIFICATION state
      break;

    case TFT_STATE_MESSAGE:
      if (currentTime - tftStateStartTime >= MESSAGE_DISPLAY_DURATION) {
        // Transition back to idle
        currentTftState = TFT_STATE_IDLE;
        displayIdleScreen(); // Directly draw idle screen upon transition
        Serial.println("TFT State: IDLE (drawn)");
      }
      // No continuous redraw within MESSAGE state
      break;
  }
}

// --- Function to manage LED flashing independently ---
void handleLedFlash() {
  if (isLedActive) {
    if (millis() - ledFlashStartTime >= LED_FLASH_DURATION) {
      digitalWrite(LED_PIN, LOW); // Turn LED off
      isLedActive = false; // Mark LED as inactive
      Serial.println("LED Off.");
    }
  }
}


// --- WebSocket Event Handler ---
void onWebSocketEvent(AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_CONNECT) {
    Serial.println("WebSocket client connected: " + String(client->id()));
    sendChatHistory(client);
    // Trigger an update to the idle screen if currently in idle state
    if (currentTftState == TFT_STATE_IDLE) {
      displayIdleScreen();
    }
  } else if (type == WS_EVT_DISCONNECT) {
    String disconnectedUser = loggedInUsers[client->id()];
    loggedInUsers.erase(client->id());
    Serial.println("WebSocket client disconnected: " + String(client->id()) + " (" + disconnectedUser + ")");
    broadcastUserList(); // This will trigger displayIdleScreen() update if in idle state
  } else if (type == WS_EVT_DATA) {
    String msg = String((char*)data).substring(0, len);
    Serial.println("Received WS message: " + msg);

    DynamicJsonDocument doc(512); // Adjust size as needed. Max 512 bytes for message.
    DeserializationError error = deserializeJson(doc, msg);

    if (error) {
      Serial.println("deserializeJson() failed: " + String(error.c_str()));
      client->text("ERROR: Invalid message format.");
      return;
    }

    String command = doc["command"].as<String>();

    if (command == "REGISTER") {
      String username = doc["username"].as<String>();
      String pass = doc["password"].as<String>();
      if (!userExists(username)) {
        registerUser(username, pass);
        client->text("STATUS:REGISTER_SUCCESS:" + username);
      } else {
        client->text("STATUS:REGISTER_FAILED:User_Exists");
      }
    } else if (command == "LOGIN") {
      String username = doc["username"].as<String>();
      String pass = doc["password"].as<String>();
      if (validateLogin(username, pass)) {
        loggedInUsers[client->id()] = username;
        broadcastUserList();
        client->text("STATUS:LOGIN_SUCCESS:" + username);
      } else {
        client->text("STATUS:LOGIN_FAILED:Invalid_Credentials");
      }
    } else if (command == "SENDTO") {
      String target = doc["target"].as<String>();
      String message = doc["message"].as<String>();
      String sender = loggedInUsers[client->id()];

      if (sender.isEmpty()) { // Check if sender is logged in
        client->text("ERROR:Not_Logged_In");
        return;
      }

      String formatted = sender + " -> " + target + ": " + message;
      appendChatHistory(formatted);

      if (target == "ALL") {
        ws.textAll(formatted);
      } else {
        bool targetFound = false;
        for (auto &pair : loggedInUsers) {
          if (pair.second == target) {
            AsyncWebSocketClient *targetClient = ws.client(pair.first);
            if (targetClient) {
              targetClient->text(formatted);
              targetFound = true;
            }
          }
        }
        if (!targetFound) {
          client->text("ERROR:Target_User_Not_Found");
        }
      }

      // --- Trigger LED flash and TFT notification ---
      digitalWrite(LED_PIN, HIGH); // Turn LED on
      isLedActive = true; // Mark LED as active
      ledFlashStartTime = millis(); // Record flash start time
      Serial.println("LED On (flash triggered).");

      setTftStateNotification(sender, message); // This sets the TFT state and draws it
    }
  }
}

void setup() {
  Serial.begin(115200);
  Serial.println("\nStarting ESP32 Chat Server...");

  // Initialize LED pin
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW); // Ensure LED is off initially
  isLedActive = false; // Confirm LED state is off

  // Initialize TFT display
  tft.initR(INITR_BLACKTAB);
  tft.fillScreen(ST77XX_BLACK); // Clear screen on startup
  tft.setTextSize(1);
  tft.setTextColor(ST77XX_GREEN);
  tft.setCursor(0, 0);
  tft.println("ESP32 Chat Starting...");

  // Setup Wi-Fi in SoftAP mode
  WiFi.softAP(ssid, password);
  IPAddress IP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(IP);

  // Attach WebSocket event handler
  ws.onEvent(onWebSocketEvent);
  server.addHandler(&ws);

  // Serve the HTML client page
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
    Serial.println("Serving HTML page.");
    request->send_P(200, "text/html", R"rawliteral(
      <!DOCTYPE html>
      <html>
      <head>
        <title>ESP32 Chat</title>
        <meta name="viewport" content="width=device-width, initial-scale=1">
        <style>
          body { font-family: Arial, sans-serif; margin: 20px; background-color: #f0f0f0; color: #333; }
          h2, h4 { color: #0056b3; }
          input[type="text"], input[type="password"] {
            padding: 8px; margin-bottom: 10px; border: 1px solid #ccc; border-radius: 4px; width: 100%; max-width: 300px;
          }
          button {
            background-color: #4CAF50; color: white; padding: 10px 15px; border: none; border-radius: 4px; cursor: pointer;
            margin-right: 5px;
          }
          button:hover { background-color: #45a049; }
          #users, #chat {
            border: 1px solid #ddd; padding: 10px; margin-top: 10px; background-color: #fff; border-radius: 5px;
            max-height: 250px; overflow-y: auto; word-wrap: break-word;
          }
          #chat div { margin-bottom: 5px; }
          select {
            padding: 8px; margin-bottom: 10px; border: 1px solid #ccc; border-radius: 4px;
          }
          .message-box {
            background-color: #e6f7ff; padding: 8px; border-radius: 5px; margin-bottom: 5px;
          }
          .error-message {
            color: red; font-weight: bold;
          }
          .success-message {
            color: green; font-weight: bold;
          }
        </style>
      </head>
      <body>
        <h2>ESP32 Chat</h2>

        <div id="statusMessage" class="error-message"></div>

        <h4>Register New User</h4>
        <input id='userR' type='text' placeholder='Username'>
        <input id='passR' type='password' placeholder='Password'>
        <button onclick='reg()'>Register</button><br><br>

        <h4>Login</h4>
        <input id='user' type='text' placeholder='Username'>
        <input id='pass' type='password' placeholder='Password'>
        <button onclick='login()'>Login</button><br><br>

        <h3>Online Users:</h3>
        <div id='users'></div><br>

        <select id='target'>
          <option value='ALL'>GROUP CHAT</option>
        </select><br><br>

        <input id='msg' type='text' placeholder='Message'>
        <button onclick='send()'>Send</button><br><br>

        <h3>Chat History:</h3>
        <div id='chat'></div>

        <script>
          var ws = new WebSocket("ws://" + location.host + "/ws");
          var username = '';
          const statusMessageElement = document.getElementById('statusMessage');

          function showStatus(message, isError = false) {
            statusMessageElement.textContent = message;
            statusMessageElement.className = isError ? 'error-message' : 'success-message';
            setTimeout(() => {
              statusMessageElement.textContent = '';
              statusMessageElement.className = '';
            }, 3000); // Clear message after 3 seconds
          }

          ws.onmessage = (e) => {
            console.log("Received: " + e.data);
            if (e.data.startsWith("USERS:")) {
              let list = e.data.substring(6).trim().split(" ");
              document.getElementById('users').innerHTML = list.filter(u => u !== '').map(u => `<div>${u}</div>`).join('');
              let sel = document.getElementById('target');
              sel.innerHTML = "<option value='ALL'>GROUP CHAT</option>";
              list.forEach(u => {
                if(u !== '' && u !== username) { // Exclude empty strings and current user
                  sel.innerHTML += `<option value="${u}">${u}</option>`;
                }
              });
            } else if (e.data.startsWith("STATUS:")) {
              let statusParts = e.data.substring(7).split(":");
              let status = statusParts[0];
              let details = statusParts[1];

              if (status === "REGISTER_SUCCESS") {
                showStatus("Registration successful for " + details, false);
              } else if (status === "REGISTER_FAILED") {
                if (details === "User_Exists") {
                  showStatus("Registration failed: Username already exists.", true);
                } else {
                  showStatus("Registration failed.", true);
                }
              } else if (status === "LOGIN_SUCCESS") {
                username = details;
                showStatus("Logged in as " + username, false);
                document.getElementById('user').value = '';
                document.getElementById('pass').value = '';
              } else if (status === "LOGIN_FAILED") {
                if (details === "Invalid_Credentials") {
                  showStatus("Login failed: Invalid username or password.", true);
                } else {
                  showStatus("Login failed.", true);
                }
              }
            } else if (e.data.startsWith("ERROR:")) {
                showStatus("Error: " + e.data.substring(6), true);
            }
            else {
              document.getElementById('chat').innerHTML += `<div class="message-box">${e.data}</div>`;
              // Scroll to bottom of chat
              var chatDiv = document.getElementById('chat');
              chatDiv.scrollTop = chatDiv.scrollHeight;
            }
          };

          function reg() {
            let u = document.getElementById('userR').value;
            let p = document.getElementById('passR').value;
            if (u && p) {
                ws.send(JSON.stringify({ command: "REGISTER", username: u, password: p }));
                document.getElementById('userR').value = '';
                document.getElementById('passR').value = '';
            } else {
                showStatus("Username and password cannot be empty for registration.", true);
            }
          }

          function login() {
            let u = document.getElementById('user').value;
            let p = document.getElementById('pass').value;
            if (u && p) {
                ws.send(JSON.stringify({ command: "LOGIN", username: u, password: p }));
            } else {
                showStatus("Username and password cannot be empty for login.", true);
            }
          }

          function send() {
            let tgt = document.getElementById('target').value;
            let msg = document.getElementById('msg').value;
            if (msg) {
                ws.send(JSON.stringify({ command: "SENDTO", target: tgt, message: msg }));
                document.getElementById('msg').value = '';
            } else {
                showStatus("Message cannot be empty.", true);
            }
          }
        </script>
      </body>
      </html>
    )rawliteral");
  });

  server.begin();
  Serial.println("HTTP server started.");
  tft.println("Server Started!"); // This will still display briefly
  currentTftState = TFT_STATE_IDLE; // Set initial state
  displayIdleScreen(); // Draw the initial idle screen
}

void loop() {
  // WebSocket cleanup (important for stable operation)
  ws.cleanupClients();

  // Continuously update the TFT display based on its current state and timers
  updateTftDisplayState(); // Renamed function

  // Handle LED flashing independently
  handleLedFlash();
}
