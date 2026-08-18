#ifndef _ROUTER_H_
#define _ROUTER_H_

#include <string>
#include <vector>
#include <functional>

class HttpRequest;
class HttpResponse;

// 轻量路由表:
// - 支持精确路径("/about")、参数路径("/kv/:key")、前缀路径("/static/*")
// - 可对指定路由开启令牌桶限流
// 路由表在首次使用时构建, 之后只读, 多线程安全
class Router {
public:
    // params: 路径段中 :xxx 捕获的参数; 前缀路由的剩余部分作为最后一个参数
    using Handler = std::function<void(const HttpRequest&, const std::vector<std::string>&, HttpResponse&)>;

    void add(const std::string& method, const std::string& pattern, Handler handler, bool rateLimited = false);

    // 匹配并执行路由; clientIp 用于限流; 返回是否已处理(含 405/429)
    bool dispatch(const HttpRequest& req, HttpResponse& resp, const std::string& clientIp) const;

private:
    struct Route {
        std::string method;
        std::vector<std::string> segments; // 按 '/' 拆分的路径段
        bool prefix;                       // 是否为 /xxx/* 前缀路由
        bool rateLimited;
        Handler handler;
    };

    // 路径与路由段匹配: 匹配时填充 params, 否则返回 false(不区分方法)
    bool matchSegments(const Route& route, const std::vector<std::string>& pathSegs,
                       std::vector<std::string>& params) const;

    std::vector<Route> _routes;
};

#endif
