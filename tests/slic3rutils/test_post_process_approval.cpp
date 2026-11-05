#include <catch2/catch_test_macros.hpp>

#include <libslic3r/AppConfig.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <slic3r/GUI/PostProcessor.hpp>

using namespace Slic3r;

namespace {
DynamicPrintConfig config_with_post_process(const std::vector<std::string>& values)
{
    DynamicPrintConfig config;
    config.set_key_value("post_process", new ConfigOptionStrings(values));
    return config;
}
} // namespace

TEST_CASE("post_process hash is empty without scripts and stable with them", "[PostProcess][Approval]")
{
    CHECK(post_process_hash(DynamicPrintConfig()).empty());
    CHECK(post_process_hash(config_with_post_process({})).empty());

    const std::string a = post_process_hash(config_with_post_process({"/bin/a"}));
    CHECK(a.size() == 64);
    CHECK(a == post_process_hash(config_with_post_process({"/bin/a"})));
    CHECK(a != post_process_hash(config_with_post_process({"/bin/b"})));
}

TEST_CASE("post_process hash separates a list from a joined string", "[PostProcess][Approval]")
{
    CHECK(post_process_hash(config_with_post_process({"a b"})) != post_process_hash(config_with_post_process({"a", "b"})));
}

TEST_CASE("only an approved post_process hash passes", "[PostProcess][Approval]")
{
    AppConfig         app_config;
    const std::string hash = post_process_hash(config_with_post_process({"/bin/a"}));

    CHECK_FALSE(is_post_process_hash_approved(app_config, hash));
    CHECK_FALSE(is_post_process_hash_approved(app_config, std::string()));

    approve_post_process_hash(app_config, hash);
    CHECK(is_post_process_hash_approved(app_config, hash));
    CHECK_FALSE(is_post_process_hash_approved(app_config, post_process_hash(config_with_post_process({"/bin/b"}))));

    approve_post_process_hash(app_config, std::string());
    CHECK_FALSE(is_post_process_hash_approved(app_config, std::string()));
}
