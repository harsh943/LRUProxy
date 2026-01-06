# Web Server with Proxy & LRU Cache

A web server implementation featuring a reverse proxy with an LRU (Least Recently Used) cache.

## About

This project implements a web server that acts as a reverse proxy, forwarding client requests to an origin server. It uses an LRU cache to store responses, reducing latency and load on the backend by serving frequently accessed content directly from memory.

## How It Works

1. Client sends a request to the server
2. Server checks if the response exists in the LRU cache
3. **Cache Hit**: Return cached response immediately
4. **Cache Miss**: Forward request to origin server, cache the response, then return it
5. When cache is full, the least recently used entry is evicted
