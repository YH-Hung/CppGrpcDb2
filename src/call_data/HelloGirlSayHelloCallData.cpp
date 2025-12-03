#include "HelloGirlSayHelloCallData.h"
#include "byte_logging.h"

IMPLEMENT_SAY_HELLO_CALLDATA_METHODS(HelloGirlSayHello, "/hellogirl.GirlGreeter/SayHello")

void HelloGirlSayHelloCallData::HandleRpc() {
    // Log the raw bytes of the name in hexadecimal (space-delimited)
    util::LogBytesHexSpaceDelimited(request_.name(), "Name bytes (hex)");
    // Also log the raw bytes of the spouse in hexadecimal (space-delimited)
    util::LogBytesHexSpaceDelimited(request_.spouse(), "Spouse bytes (hex)");

    std::string prefix("Hello ");
    reply_.set_message(prefix + request_.name());

    // Compose additional fields as requested
    std::string marriage = request_.name() + " is married with " + request_.spouse();
    reply_.set_marriage(marriage);
    reply_.set_size(request_.first_round() + 1);
}

