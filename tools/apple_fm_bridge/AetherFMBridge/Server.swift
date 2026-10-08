import Foundation
import Network

/// A minimal HTTP/1.1 server for the benchmark harness: one request per
/// connection, JSON in and out, and a bearer token on every route except
/// GET /health.
///
///   GET  /health                 {"ok": true}
///   GET  /v1/models              the served models, availability and context sizes
///   POST /v1/chat/completions    OpenAI-shaped chat completion
final class BridgeServer: @unchecked Sendable {
    let port: UInt16
    let token: String
    private let engine: InferenceEngine
    private var listener: NWListener?
    private let queue = DispatchQueue(label: "aether.fm.bridge")
    var onLog: (@Sendable (String) -> Void)?

    init(port: UInt16, token: String, engine: InferenceEngine) {
        self.port = port
        self.token = token
        self.engine = engine
    }

    func start() throws {
        let params = NWParameters.tcp
        params.allowLocalEndpointReuse = true
        let listener = try NWListener(using: params, on: NWEndpoint.Port(rawValue: port)!)
        listener.newConnectionHandler = { [weak self] connection in
            self?.serve(connection)
        }
        listener.stateUpdateHandler = { [weak self] state in
            self?.onLog?("listener: \(state)")
        }
        listener.start(queue: queue)
        self.listener = listener
    }

    private func serve(_ connection: NWConnection) {
        connection.start(queue: queue)
        receive(connection, buffer: Data())
    }

    private func receive(_ connection: NWConnection, buffer: Data) {
        connection.receive(minimumIncompleteLength: 1, maximumLength: 1 << 20) { [weak self] data, _, done, error in
            guard let self else { return }
            var buffer = buffer
            if let data { buffer.append(data) }
            if let request = HTTPRequest(parsing: buffer) {
                Task { await self.respond(to: request, on: connection) }
            } else if done || error != nil || buffer.count > 64 << 20 {
                connection.cancel()
            } else {
                self.receive(connection, buffer: buffer)
            }
        }
    }

    private func respond(to request: HTTPRequest, on connection: NWConnection) async {
        let reply: BridgeReply
        if request.method == "GET" && request.path == "/health" {
            reply = BridgeReply(status: 200, body: ["ok": true])
        } else if request.headers["authorization"] != "Bearer \(token)" {
            reply = BridgeReply(status: 401, body: ["error": ["code": "unauthorized", "message": "bearer token required"]])
        } else if request.method == "GET" && request.path == "/v1/models" {
            reply = BridgeReply(status: 200, body: ["object": "list", "data": await engine.describeModels()])
        } else if request.method == "POST" && request.path == "/v1/chat/completions" {
            do {
                let chat = try JSONDecoder().decode(ChatRequest.self, from: request.body)
                onLog?("chat \(chat.model ?? "apple-on-device"): \(request.body.count) bytes")
                reply = await engine.complete(chat)
                onLog?("-> \(reply.status)")
            } catch {
                reply = BridgeReply(status: 400, body: ["error": ["code": "invalid_json", "message": "\(error)"]])
            }
        } else {
            reply = BridgeReply(status: 404, body: ["error": ["code": "not_found", "message": "\(request.method) \(request.path)"]])
        }
        send(reply, on: connection)
    }

    private func send(_ reply: BridgeReply, on connection: NWConnection) {
        let body = (try? JSONSerialization.data(withJSONObject: reply.body, options: [.sortedKeys])) ?? Data("{}".utf8)
        let reason = [200: "OK", 400: "Bad Request", 401: "Unauthorized", 404: "Not Found",
                      429: "Too Many Requests", 500: "Internal Server Error", 503: "Service Unavailable"][reply.status] ?? "Status"
        var head = "HTTP/1.1 \(reply.status) \(reason)\r\n"
        head += "Content-Type: application/json\r\nContent-Length: \(body.count)\r\nConnection: close\r\n\r\n"
        connection.send(content: Data(head.utf8) + body, completion: .contentProcessed { _ in connection.cancel() })
    }
}

/// A request is complete once its headers and Content-Length bytes are in.
struct HTTPRequest {
    let method: String
    let path: String
    let headers: [String: String]
    let body: Data

    init?(parsing data: Data) {
        guard let split = data.range(of: Data("\r\n\r\n".utf8)),
              let head = String(data: data[..<split.lowerBound], encoding: .utf8) else { return nil }
        var lines = head.components(separatedBy: "\r\n")
        let start = lines.removeFirst().split(separator: " ")
        guard start.count >= 2 else { return nil }
        var headers: [String: String] = [:]
        for line in lines {
            guard let colon = line.firstIndex(of: ":") else { continue }
            headers[line[..<colon].lowercased()] = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
        }
        let length = Int(headers["content-length"] ?? "0") ?? 0
        let body = data[split.upperBound...]
        guard body.count >= length else { return nil }
        self.method = String(start[0])
        self.path = String(start[1].split(separator: "?").first ?? "")
        self.headers = headers
        self.body = Data(body.prefix(length))
    }
}
