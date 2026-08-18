#ifndef _HTTPREQUEST_H_
#define _HTTPREQUEST_H_

#include <string>
#include <map>

// 解析结果
enum class ParseResult {
    NEED_MORE,  // 数据不完整(半包), 等待更多数据
    OK,         // 成功解析出一个完整请求
    ERROR       // 格式错误或超出限制
};

// HTTP 请求解析器(无状态, 每次解析一个完整请求):
// 输入缓冲由连接对象持有, 解析器不保存跨请求状态,
// 因此天然支持"半包累积、跨事件解析"与流水线多请求
class HttpRequest {
public:
    // 报文尺寸上限, 防止恶意超长报文耗尽内存
    static const size_t MAX_REQUEST_LINE = 8192;
    static const size_t MAX_HEADER_TOTAL = 65536;
    static const size_t MAX_BODY = 16 * 1024 * 1024;

    HttpRequest();
    ~HttpRequest();

    void clear();

    // 从累积缓冲 raw 中解析一个完整请求
    // 成功时 consumed = 该请求(请求行+头部+空行+请求体)占用的字节数
    ParseResult parse(const std::string& raw, size_t& consumed);

    const std::string& getMethod() const;
    const std::string& getPath() const;
    const std::string& getVersion() const;
    const std::string& getQueryParam(const std::string& key) const;
    const std::string& getHeader(const std::string& key) const;
    const std::map<std::string, std::string>& getAllHeaders() const;
    const std::string& getBody() const;

    // 根据 HTTP 版本与 Connection 头计算连接是否保持
    bool keepAlive() const;

    // 解析失败时建议返回的响应码(400/413/431/501)
    int errorCode() const;

private:
    bool parseRequestLine(const std::string& line);
    bool parseHeaderLine(const std::string& line);
    void parseQueryParams(const std::string& queryStr);

    std::string _method;
    std::string _path;
    std::string _version;
    std::map<std::string, std::string> _headers;
    std::map<std::string, std::string> _queryParams;
    std::string _body;
    bool _keepAlive;
    int _errorCode;
};

#endif
