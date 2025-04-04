# Multi-Threaded HTTP Server

## Description

This is a simple HTTP server designed to handle incoming HTTP requests, specifically GET and PUT
methods. It follows a simple request-response model, where it listens for incoming connections,
parses the requests, processes them accordingly, and sends back appropriate reponses. 

## Features 

- **Suports GET and PUT HTTP methods**
- **Responds to various HTTP status codes**
- **Handles basic error cases gracefully.**

## Components

### 1. Main Functionality:

- The 'main' function initializesd the server by setting up a listener socket on the specified port and enters a loop to accept incoming connections

### 2. Request Parsing:

- The 'parsing_request' function parses the incoming HTTP request to extract the method, path, and HTTP version.
- It also extracts the content length for PUT requests, if present.
- Invalid requests or unsupported HTTP versions are handled by sending appropriate error reponses.

### 3. HTTP Methods Handling:

- Ther server supports GET and PUT methods.
- For GET requests, the server checks if the requested resource is a file, reads its contentc, and sends them back to the client.
- For PUT requests, the server checks if the requested resource is valid file path, creates or truncates the file, and writes the data received from the client to file. 
