#include "HelloGirlSayHelloCallData.h"

#include <utf8ansi.h>

#include "byte_logging.h"

IMPLEMENT_SAY_HELLO_CALLDATA_METHODS(HelloGirlSayHello, "/hellogirl.GirlGreeter/SayHello")

void HelloGirlSayHelloCallData::HandleRpc() {
    // Log the raw bytes of the name in hexadecimal (space-delimited)
    util::LogBytesHexSpaceDelimited(request_.name(), "Name bytes (hex)");
    // Also log the raw bytes of the spouse in hexadecimal (space-delimited)
    util::LogBytesHexSpaceDelimited(request_.spouse(), "Spouse bytes (hex)");
    // Log raw bytes field
    util::LogBytesHexSpaceDelimited(request_.secret_note(), "Secret note bytes (hex)");

    std::string prefix("Hello ");
    reply_.set_message(prefix + request_.name());

    // Compose additional fields as requested
    std::string marriage = request_.name() + " is married with " + request_.spouse();
    reply_.set_marriage(marriage);
    reply_.set_size(request_.first_round() + 1);

    // Handle bytes field
    std::string big5_append = utf8ansi::utf8_to_big5("大好");
    std::string secret_reply = request_.secret_note() + "is " + big5_append;
    reply_.set_reply_secret(secret_reply);
}

