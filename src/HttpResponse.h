#ifndef _HTTPRESPONSE_H_
#define _HTTPRESPONSE_H_

#include <string>
#include <map>

// HTTP 响应构造器: 组装状态行 + 响应头 + 响应体
class HttpResponse {
public:
    HttpResponse();
    ~HttpResponse();

    void setStatusCode(int code);
    void setStatusMessage(const std::string& message);
    void setVersion(const std::string& version);

    // 设置状态码并自动匹配标准原因短语
    void setStatus(int code);

    void addHeader(const std::string& key, const std::string& value);
    void setBody(const std::string& body);
    void setBody(const char* body, size_t length);

    // 序列化为完整 HTTP 响应报文
    std::string toString() const;

    void clear();

private:
    int _statusCode;
    std::string _statusMessage;
    std::string _version;
    std::map<std::string, std::string> _headers;
    std::string _body;
};

#endif
