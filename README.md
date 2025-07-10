binance_bot/
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
├── secrets.conf
├── Makefile
└── trades.csv


sudo apt update
sudo apt install -y build-essential libcurl4-openssl-dev libssl-dev libboost-system-dev libasio-dev libssl-dev cmake git libwebsocketpp-dev


mkdir build && cd build
cmake ..
make
./client
