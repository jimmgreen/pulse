#pragma once

#include "picker_transport.h"

namespace pulse::ipc::picker {

// The authenticated channel binds the peer's process and lifetime. Its request
// ID identifies this single Show; no UI request is sent before the welcome.
inline TransportResult NegotiatePickerSession(Channel& channel, const Message& request,
                                             WaitOptions wait = {nullptr, 10000}) {
    const auto* body = std::get_if<Request>(&request.body);
    if (!body) return {ERROR_INVALID_DATA};
    SessionHello hello;
    hello.required_capabilities = body->required_capabilities;
    Message offer{request.request_id, hello};
    auto result = channel.Send(offer, wait);
    Message welcome;
    if (result) result = channel.Receive(welcome, wait);
    if (!result) return result;
    if (welcome.request_id != request.request_id) return {ERROR_INVALID_DATA};
    if (const auto* reply = std::get_if<SessionWelcome>(&welcome.body); reply && !reply->version)
        return {ERROR_NOT_SUPPORTED};
    return ValidateSessionWelcome(offer, welcome) ? TransportResult{} : TransportResult{ERROR_INVALID_DATA};
}

// Both cold and standby hosts use the same gate, before preparing any UI.
// Legacy callers remain supported. Failure leaves the output untouched.
inline TransportResult ReceivePickerSessionRequest(Channel& channel, Message& output,
                                                  WaitOptions wait = {nullptr, 10000}) {
    Message first;
    auto result = channel.Receive(first, wait);
    if (!result) return result;
    if (std::holds_alternative<Request>(first.body)) {
        output = std::move(first);
        return {};
    }
    if (!std::holds_alternative<SessionHello>(first.body)) return {ERROR_INVALID_DATA};
    const auto welcome = MakeSessionWelcome(first);
    result = channel.Send(welcome, wait);
    if (!result) return result;
    if (!ValidateSessionWelcome(first, welcome)) return {ERROR_NOT_SUPPORTED};
    Message request;
    result = channel.Receive(request, wait);
    if (!result) return result;
    if (request.request_id == first.request_id && std::holds_alternative<Cancel>(request.body))
        return {ERROR_CANCELLED};
    if (!ValidateSessionRequest(first, welcome, request)) return {ERROR_INVALID_DATA};
    output = std::move(request);
    return {};
}

} // namespace pulse::ipc::picker
