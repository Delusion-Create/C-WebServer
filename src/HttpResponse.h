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

    // 标记为文件响应: 序列化时只输出状态行+头部, 文件体由 sendfile 零拷贝发送
    void setFile(const std::string& path);
    bool hasFile() const;
    const std::string& filePath() const;

    // 序列化为完整 HTTP 响应报文(含请求体)
    std::string toString() const;

    // 只序列化状态行 + 响应头 + 空行, 不含响应体(用于文件响应)
    std::string toStringHeaders() const;

    void clear();

private:
    int _statusCode;
    std::string _statusMessage;
    std::string _version;
    std::map<std::string, std::string> _headers;
    std::string _body;
    bool _hasFile;
    std::string _filePath;
};

#endif
