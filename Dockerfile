FROM ubuntu:22.04
RUN apt-get update && apt-get install -y g++ make
WORKDIR /app
COPY sensor_parser.cpp json.hpp config.json ./
RUN g++ -std=c++17 -I. sensor_parser.cpp -o parser
ENTRYPOINT ["./parser"]