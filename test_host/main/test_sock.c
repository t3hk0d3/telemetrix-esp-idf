#include <arpa/inet.h>
#include <netinet/in.h>
#include <string.h>

#include "tmx_net.h"
#include "unity.h"

static struct sockaddr_in v4(const char *text, uint16_t port)
{
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    TEST_ASSERT_EQUAL(1, inet_pton(AF_INET, text, &a.sin_addr));
    return a;
}

static struct sockaddr_in6 v6(const char *text, uint16_t port)
{
    struct sockaddr_in6 a;
    memset(&a, 0, sizeof(a));
    a.sin6_family = AF_INET6;
    a.sin6_port = htons(port);
    TEST_ASSERT_EQUAL(1, inet_pton(AF_INET6, text, &a.sin6_addr));
    return a;
}

static telemetrix_config_t with_list(const telemetrix_cidr_t *list, size_t n)
{
    telemetrix_config_t c;
    telemetrix_config_default(&c);
    c.allowed = list;
    c.n_allowed = n;
    return c;
}

static telemetrix_cidr_t cidr(const char *text)
{
    telemetrix_cidr_t c;
    TEST_ASSERT_EQUAL(ESP_OK, telemetrix_cidr_parse(text, &c));
    return c;
}

TEST_CASE("allowed with an empty list: any peer", "[sock]")
{
    telemetrix_config_t c = with_list(NULL, 0);
    struct sockaddr_in a = v4("8.8.8.8", 1);
    TEST_ASSERT_TRUE(tmx_sock_allowed(&c, (struct sockaddr *)&a));
}

TEST_CASE("allowed with an IPv4 network: members in, others out", "[sock]")
{
    telemetrix_cidr_t list[] = {cidr("192.168.10.0/23")};
    telemetrix_config_t c = with_list(list, 1);
    struct sockaddr_in in1 = v4("192.168.11.200", 1), in2 = v4("192.168.10.1", 1), out = v4("192.168.12.1", 1);
    TEST_ASSERT_TRUE(tmx_sock_allowed(&c, (struct sockaddr *)&in1));
    TEST_ASSERT_TRUE(tmx_sock_allowed(&c, (struct sockaddr *)&in2));
    TEST_ASSERT_FALSE(tmx_sock_allowed(&c, (struct sockaddr *)&out));
}

TEST_CASE("allowed with an IPv6 network and an IPv4 host: each family matches its own entries", "[sock]")
{
    telemetrix_cidr_t list[] = {cidr("fd00:1::/32"), cidr("10.0.0.5")};
    telemetrix_config_t c = with_list(list, 2);
    struct sockaddr_in6 in6 = v6("fd00:1:abcd::7", 1), out6 = v6("fd00:2::1", 1);
    struct sockaddr_in in4 = v4("10.0.0.5", 1), out4 = v4("10.0.0.6", 1);
    TEST_ASSERT_TRUE(tmx_sock_allowed(&c, (struct sockaddr *)&in6));
    TEST_ASSERT_FALSE(tmx_sock_allowed(&c, (struct sockaddr *)&out6));
    TEST_ASSERT_TRUE(tmx_sock_allowed(&c, (struct sockaddr *)&in4));
    TEST_ASSERT_FALSE(tmx_sock_allowed(&c, (struct sockaddr *)&out4));
}

TEST_CASE("normalise of an IPv4-mapped IPv6 peer: a plain IPv4 peer that the IPv4 list matches", "[sock]")
{
    telemetrix_cidr_t list[] = {cidr("172.16.0.0/12")};
    telemetrix_config_t c = with_list(list, 1);
    struct sockaddr_in6 mapped = v6("::ffff:172.20.1.9", 4242);
    struct sockaddr_storage in, out;
    memset(&in, 0, sizeof(in));
    memcpy(&in, &mapped, sizeof(mapped));
    tmx_sock_normalise(&in, &out);
    TEST_ASSERT_EQUAL(AF_INET, out.ss_family);
    TEST_ASSERT_TRUE(tmx_sock_allowed(&c, (struct sockaddr *)&out));
    char text[TELEMETRIX_PEER_MAX];
    tmx_sock_peer_text((struct sockaddr *)&out, text);
    TEST_ASSERT_EQUAL_STRING("172.20.1.9:4242", text);
}

TEST_CASE("normalise of a real IPv6 peer: unchanged", "[sock]")
{
    struct sockaddr_in6 a = v6("2001:db8::1", 80);
    struct sockaddr_storage in, out;
    memset(&in, 0, sizeof(in));
    memcpy(&in, &a, sizeof(a));
    tmx_sock_normalise(&in, &out);
    TEST_ASSERT_EQUAL(AF_INET6, out.ss_family);
    char text[TELEMETRIX_PEER_MAX];
    tmx_sock_peer_text((struct sockaddr *)&out, text);
    TEST_ASSERT_EQUAL_STRING("[2001:db8::1]:80", text);
}

TEST_CASE("peer_text of the longest IPv6 peer: fits TELEMETRIX_PEER_MAX", "[sock]")
{
    struct sockaddr_in6 a = v6("ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffe", 65535);
    char text[TELEMETRIX_PEER_MAX];
    tmx_sock_peer_text((struct sockaddr *)&a, text);
    TEST_ASSERT_EQUAL_STRING("[ffff:ffff:ffff:ffff:ffff:ffff:ffff:fffe]:65535", text);
}
