import Foundation
import FoundationModels

/// One OpenAI-style chat request, decoded from the harness's JSON.
struct ChatRequest: Decodable {
    struct Message: Decodable {
        let role: String
        let content: String
    }
    let model: String?
    let messages: [Message]
    let temperature: Double?
    let max_tokens: Int?
    let max_completion_tokens: Int?
    let seed: UInt64?
    /// Bridge-only switches, for isolating framework trouble:
    /// {"sampling": "default|greedy|nucleus", "max_tokens": true|false}.
    let bridge_options: [String: String]?
}

/// What the bridge answers with: an HTTP status and a JSON body.
struct BridgeReply {
    let status: Int
    let body: [String: Any]
}

/// The models the bridge serves. `apple-on-device` is the system model the
/// device ships (its variant, e.g. core3 or coreAdvanced3, is reported, not
/// chosen). `apple-pcc*` is Private Cloud Compute, optionally at a reasoning level.
enum BridgeModel: String, CaseIterable {
    case onDevice = "apple-on-device"
    case pcc = "apple-pcc"
    case pccLight = "apple-pcc-light"
    case pccModerate = "apple-pcc-moderate"
    case pccDeep = "apple-pcc-deep"

    var isPCC: Bool { self != .onDevice }

    var reasoningLevel: ContextOptions.ReasoningLevel? {
        switch self {
        case .pccLight: return .light
        case .pccModerate: return .moderate
        case .pccDeep: return .deep
        default: return nil
        }
    }
}

/// Serialises generation: one request at a time, so timings are clean and the
/// on-device model is never asked for two sessions at once.
actor InferenceEngine {
    private(set) var served = 0
    private(set) var lastError: String = ""

    func describeModels() async -> [[String: Any]] {
        var out: [[String: Any]] = []
        let system = SystemLanguageModel.default
        var device: [String: Any] = [
            "id": BridgeModel.onDevice.rawValue,
            "object": "model",
            "available": system.isAvailable,
            "availability": String(describing: system.availability),
            "variant": system.variant.displayName,
            "context_length": system.contextSize,
        ]
        if !system.isAvailable { device["note"] = "enable Apple Intelligence in Settings" }
        out.append(device)
        let pcc = PrivateCloudComputeLanguageModel()
        let pccContext = (try? await pcc.contextSize) ?? 0
        for model in BridgeModel.allCases where model.isPCC {
            out.append([
                "id": model.rawValue,
                "object": "model",
                "availability": String(describing: pcc.availability),
                "quota": String(describing: pcc.quotaUsage),
                "context_length": pccContext,
            ])
        }
        return out
    }

    func complete(_ request: ChatRequest) async -> BridgeReply {
        let name = request.model ?? BridgeModel.onDevice.rawValue
        guard let model = BridgeModel(rawValue: name) else {
            return error(400, "model_not_found", "unknown model \(name); see GET /v1/models")
        }
        #if !AETHER_FM_PCC
        // Without Apple's managed PCC entitlement, the first PCC generation hits
        // an assertion inside FoundationModels and kills the app (2026-10-08).
        // Build with AETHER_FM_PCC once the entitlement is granted.
        if model.isPCC {
            return error(503, "pcc_not_entitled",
                         "this build has no Private Cloud Compute entitlement; see developer.apple.com/private-cloud-compute")
        }
        #endif
        // System messages become the session instructions. Earlier turns are
        // folded into the prompt as a transcript, so a repair conversation
        // keeps its history in one call.
        let instructions = request.messages.filter { $0.role == "system" }.map(\.content).joined(separator: "\n\n")
        let turns = request.messages.filter { $0.role != "system" }
        guard let last = turns.last, last.role == "user" else {
            return error(400, "invalid_request", "the last message must be a user message")
        }
        var prompt = ""
        for turn in turns.dropLast() {
            prompt += "\(turn.role == "assistant" ? "Assistant" : "User"):\n\(turn.content)\n\n"
        }
        prompt += turns.count > 1 ? "User:\n\(last.content)" : last.content

        let temperature = request.temperature ?? 0.2
        let knobs = request.bridge_options ?? [:]
        // PCC tripped an assertion inside FoundationModels on the first request
        // (2026-10-08), so by default it gets only a temperature: no explicit
        // sampling mode and no token cap. Each can be switched back on per request.
        let samplingKnob = knobs["sampling"] ?? (model.isPCC ? "default" : "nucleus")
        let sampling: GenerationOptions.SamplingMode?
        switch samplingKnob {
        case "greedy": sampling = .greedy
        case "nucleus": sampling = temperature <= 0 ? .greedy : .random(probabilityThreshold: 0.95, seed: request.seed)
        default: sampling = nil
        }
        let capTokens = (knobs["max_tokens"] ?? (model.isPCC ? "false" : "true")) == "true"
        let options = GenerationOptions(
            sampling: sampling,
            temperature: temperature <= 0 ? nil : temperature,
            maximumResponseTokens: capTokens ? (request.max_completion_tokens ?? request.max_tokens) : nil
        )
        RequestLog.write("model=\(model.rawValue) sampling=\(samplingKnob) seed=\(request.seed.map(String.init) ?? "nil") "
                         + "temperature=\(temperature) cap=\(capTokens ? String(describing: options.maximumResponseTokens) : "none") "
                         + "reasoning=\(String(describing: model.reasoningLevel)) prompt_chars=\(prompt.count) instr_chars=\(instructions.count)")

        let started = Date()
        do {
            let text: String
            var promptTokens: Int? = nil
            if model.isPCC {
                let session = LanguageModelSession(model: PrivateCloudComputeLanguageModel(),
                                                   instructions: instructions.isEmpty ? nil : instructions)
                if let level = model.reasoningLevel {
                    text = try await session.respond(to: prompt, options: options,
                                                     contextOptions: ContextOptions(reasoningLevel: level)).content
                } else {
                    text = try await session.respond(to: prompt, options: options).content
                }
            } else {
                let system = SystemLanguageModel.default
                guard system.isAvailable else {
                    return error(503, "model_unavailable", "on-device model unavailable: \(system.availability)")
                }
                promptTokens = try? await system.tokenCount(for: prompt)
                let session = LanguageModelSession(model: system,
                                                   instructions: instructions.isEmpty ? nil : instructions)
                text = try await session.respond(to: prompt, options: options).content
            }
            served += 1
            RequestLog.write("ok \(text.count) chars")
            var usage: [String: Any] = [:]
            if let promptTokens { usage["prompt_tokens"] = promptTokens }
            return BridgeReply(status: 200, body: [
                "id": "apple-fm-\(UUID().uuidString)",
                "object": "chat.completion",
                "created": Int(started.timeIntervalSince1970),
                "model": model.rawValue,
                "choices": [[
                    "index": 0,
                    "message": ["role": "assistant", "content": text],
                    "finish_reason": "stop",
                ]],
                "usage": usage,
                "bridge": ["seconds": Date().timeIntervalSince(started),
                           "sampling": temperature <= 0 ? "greedy" : "nucleus 0.95"],
            ])
        } catch let generation as LanguageModelSession.GenerationError {
            return map(generation)
        } catch let pccError as PrivateCloudComputeLanguageModel.Error {
            // Quota, network or service trouble measured nothing: report it as
            // an infrastructure failure so the harness re-runs the case.
            return error(503, "pcc_unavailable", String(describing: pccError))
        } catch {
            return self.error(500, "bridge_error", String(describing: error))
        }
    }

    private func map(_ generation: LanguageModelSession.GenerationError) -> BridgeReply {
        switch generation {
        case .exceededContextWindowSize:
            return error(400, "context_length_exceeded", String(describing: generation))
        case .guardrailViolation, .refusal:
            // A refusal is the model's answer, not an outage: an empty program
            // the harness then scores as a failure.
            served += 1
            return BridgeReply(status: 200, body: [
                "id": "apple-fm-\(UUID().uuidString)",
                "object": "chat.completion",
                "choices": [["index": 0, "message": ["role": "assistant", "content": ""],
                             "finish_reason": "content_filter"]],
                "bridge": ["refusal": String(describing: generation)],
            ])
        case .rateLimited:
            return error(429, "rate_limited", String(describing: generation))
        default:
            return error(503, "generation_unavailable", String(describing: generation))
        }
    }

    private func error(_ status: Int, _ code: String, _ message: String) -> BridgeReply {
        lastError = "\(code): \(message)"
        return BridgeReply(status: status, body: ["error": ["code": code, "message": message]])
    }
}

/// The last requests' options, appended to Documents/requests.log before each
/// generation so a crash inside the framework leaves the request that caused it.
enum RequestLog {
    static func write(_ line: String) {
        guard let dir = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask).first else { return }
        let url = dir.appendingPathComponent("requests.log")
        let stamped = "\(Date().ISO8601Format()) \(line)\n"
        if let handle = try? FileHandle(forWritingTo: url) {
            handle.seekToEndOfFile()
            handle.write(Data(stamped.utf8))
            try? handle.synchronize()
            try? handle.close()
        } else {
            try? Data(stamped.utf8).write(to: url)
        }
    }
}
