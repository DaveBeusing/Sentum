# 📈 Sentum  
**Intelligent Signals. Real-Time Decisions. Confident Trading.**

## 🔍 Overview
**Sentum** is a high-performance, C++17-based trading bot tailored for real-time signal processing and automated trading strategies. With a clean modular design and secure WebSocket integration to Binance, it delivers precise market reactions in milliseconds.

> _Built for precision. Designed for performance. Engineered for markets._  

## Key Features
- 🔌 Real-time data via Binance WebSocket API
- 📊 Technical indicators (e.g., SMA crossover) built in
- 🧠 Strategy modules are modular and easily extendable
- 🔐 TLS-secured communication using OpenSSL
- ⚡ Optimized for speed and reliability with C++17
- 🧩 Lightweight, no unnecessary dependencies


## Technology Stack
| Component         | Technology                   |
|------------------|------------------------------|
| Language          | C++17                        |
| WebSocket Client  | [WebSocket++](https://github.com/zaphoyd/websocketpp) |
| TLS Support       | OpenSSL                      |
| JSON Parsing      | [nlohmann/json](https://github.com/nlohmann/json) |
| Build System      | CMake & Make                 |
| Exchange          | Binance (Spot Market)        |

## Project Structure
```
sentum/
│── build/
│── config/
│	├── risk.json
│	└── secrets.json
├── src/
│	├── nlohmann/
│	│	└── json.hpp
│	├── sentum/
│	│	├── api/
│	│	│	├── binance.cpp
│	│	│	└── binance.hpp
│	│	├── chart/
│	│	│	├── chart.cpp
│	│	│	└── chart.hpp
│	│	├── core/
│	│	│	├── trader.cpp
│	│	│	└── trader.hpp
│	│	├── strategy/
│	│	│	├── rsi.cpp
│	│	│	├── rsi.hpp
│	│	│	├── sma.cpp
│	│	│	├── sma.hpp
│	│	│	├── strategy.cpp
│	│	│	└── strategy.hpp
│	│	├── utils/
│	│	│	├── config.cpp
│	│	│	├── config.hpp
│	│	│	├── secrets.cpp
│	│	│	├── secrets.hpp
│	│	│	└── style.hpp
│	│	└── websocket/
│	│		├── wsclient.cpp
│	│		└── wsclient.hpp
│	└── main.cpp
|── .gitignore
|── clean.sh
|── CMakeLists.txt
|── DISCLAIMER.md
|── LICENSE.md
└── README.md

example secrets.json
{
	"api_key": "your_binance_api_key",
	"api_secret": "your_binance_api_secret"
}

```

## Quick Start
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

## Roadmap
- [ ] WebSocket auto-reconnect logic
- [ ] Configurable strategies (via JSON/YAML)
- [ ] Historical backtesting module
- [ ] REST order execution support
- [ ] Dashboard UI for live strategy monitoring

## Contributing
Pull requests are welcome. For major changes, please open an issue first to discuss what you would like to change or propose.
If you find this project useful consider starring the repo or sharing it with fellow devs and traders!

## ⚠️ DISCLAIMER ⚠️
The Content is for informational purposes only, you should not construe any such information or other material as legal, tax, investment, financial, or other advice.
<br><br>
<b>Please read and understand DISCLAIMER.md in addition to the aforementioned disclaimer.</b>

<b>You need to have a basic understanding of the topic and be hands-on to make this piece of code profitable, otherwise you will lose your money, be warned!</b>

## License
Copyright ©️ 2025 Dave Beusing

MIT License - https://opensource.org/license/mit/

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the “Software”), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished 
to do so, subject to the following conditions:
The above copyright notice and this permission notice shall be included in all 
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED “AS IS”, WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED,
INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A 
PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT 
HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION 
OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE 
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.


---

> _Built for precision. Designed for performance. Engineered for markets._  
> — **Sentum**