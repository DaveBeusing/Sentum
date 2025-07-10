# 📈 Sentum  
**Intelligent Signals. Real-Time Decisions. Confident Trading.**


## 🔍 Overview

**Sentum** is a high-performance, C++17-based trading bot tailored for real-time signal processing and automated trading strategies. With a clean modular design and secure WebSocket integration to Binance, it delivers precise market reactions in milliseconds.

Built for developers, quants, and fintech teams who demand **control, performance, and reliability**.


## ⚙️ Key Features
- 🔌 Real-time data via Binance WebSocket API
- 📊 Technical indicators (e.g., SMA crossover) built in
- 🧠 Strategy modules are modular and easily extendable
- 🔐 TLS-secured communication using OpenSSL
- ⚡ Optimized for speed and reliability with C++17
- 🧩 Lightweight, no unnecessary dependencies


## 🛠️ Technology Stack
| Component         | Technology                   |
|------------------|------------------------------|
| Language          | C++17                        |
| WebSocket Client  | [WebSocket++](https://github.com/zaphoyd/websocketpp) |
| TLS Support       | OpenSSL                      |
| JSON Parsing      | [nlohmann/json](https://github.com/nlohmann/json) |
| Build System      | CMake & Make                 |
| Exchange          | Binance (Spot Market)        |

## 🧱 Project Structure
```
sentum/
├── include/
│   ├── api.hpp
│   ├── config.hpp
│   ├── logger.hpp
│   ├── risk.hpp
│   └── strategy.hpp
├── src/
│   ├── api.cpp
│   ├── logger.cpp
│   ├── risk.cpp
│   ├── strategy.cpp
│   └── main.cpp
├── lib/json.hpp
|── build/
|   ├── client
|   ├── trades.csv
└── secrets.conf

/src           → Source code 
/include       → Headers
/lib           → External libraries (e.g. json)
/build         → Build artifacts (ignored in Git)
/secrets.conf

[binance]
api_key=your_binance_api_key
secret_key=your_binance_api_secret

```

## 🚀 Quick Start

### 1. Prepare your system
```bash
sudo apt update
sudo apt install -y build-essential libcurl4-openssl-dev libssl-dev libboost-system-dev libasio-dev libssl-dev cmake git libwebsocketpp-dev
```

### 2. Clone the repository
```bash
git clone https://github.com/DaveBeusing/Sentum.git
cd sentum
```

### 3. Build
```bash
mkdir build && cd build
cmake ..
make
```

### 4. Run
```bash
./client
```


## 🔒 Security & Design Principles
> **Sentum** follows the principle: _Strategies can change, stability must not._
- Graceful error handling and reconnect logic
- Secrets (API keys) stored outside of version control
- Code built for readability and extendability


## 👤 Target Audience
- Algorithmic traders and quantitative analysts
- C++ developers in fintech and trading
- Teams building low-latency trading infrastructure
- Open-source contributors with market experience

## 🚧 Roadmap
- [ ] WebSocket auto-reconnect logic
- [ ] Configurable strategies (via JSON/YAML)
- [ ] Historical backtesting module
- [ ] REST order execution support
- [ ] Dashboard UI for live strategy monitoring


## 📜 License
This project is licensed under the MIT License.  
All source files include license headers.

## 🤝 Contributing
Pull requests are welcome. For major changes, please open an issue first to discuss what you would like to change or propose.

## ⭐ If you find this project useful...
...consider starring the repo or sharing it with fellow devs and traders!

## ⚠️ DISCLAIMER ⚠️
The Content is for informational purposes only, you should not construe any such information or other material as legal, tax, investment, financial, or other advice.
<br><br>
<b>Please read and understand DISCLAIMER.md in addition to the aforementioned disclaimer.</b>

<b>You need to have a basic understanding of the topic and be hands-on to make this piece of code profitable, otherwise you will lose your money, be warned!</b>


---

> _Built for precision. Designed for performance. Engineered for markets._  
> — **Sentum**