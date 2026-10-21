#include "discord/read_state.hpp"
#include <cassert>
#include <iostream>

int main()
{
    using nlohmann::json;
    for (const char *input : {"null", "{}", "{\"mentions\":null}",
                              "{\"mentions\":\"3\"}", "{\"mentions\":false}",
                              "{\"mentions\":-1}", "{\"mentions\":1e100}"})
        assert(discord::mention_count(json::parse(input)) == 0);
    assert(discord::mention_count(json::parse("{\"mentions\":3}")) == 3);
    assert(discord::mention_count(json::parse("{\"mentions\":3.0}")) == 3);
    std::cout << "client read state: missing, null, malformed and valid counts passed\n";
}
