#include "HttpRequest.h"
#include <sstream>
#include <cctype>

HttpRequest::HttpRequest() { clear(); }
HttpRequest::~HttpRequest() {}

void HttpRequest::clear()
{
    _method.clear();
    _path.clear();
    _version.clear();
    _headers.clear();
    _queryParams.clear();
    _body.clear();
    _keepAlive = false;
    _errorCode = 400;
}

ParseResult HttpRequest::parse(const std::string& raw, size_t& consumed)
{
    clear();
    consumed = 0;

    // 1. 定位请求头结束位置(空行 \r\n\r\n)
    size_t headerEnd = raw.find("\r\n\r\n");
    if (headerEnd == std::string::npos) {
        if (raw.size() > MAX_HEADER_TOTAL) {
            _errorCode = 431; // 头部超限
            return ParseResult::ERROR;
        }
        return ParseResult::NEED_MORE;
    }

    // 2. 解析请求行
    size_t lineEnd = raw.find("\r\n");
    if (lineEnd == std::string::npos || lineEnd > MAX_REQUEST_LINE) {
        _errorCode = 431;
        return ParseResult::ERROR;
    }
    if (!parseRequestLine(raw.substr(0, lineEnd))) {
        return ParseResult::ERROR;
    }

    // 3. 解析请求头(可能没有头部, 直接到空行)
    size_t pos = lineEnd + 2;
    while (pos < headerEnd) {
        size_t eol = raw.find("\r\n", pos);
        if (eol == std::string::npos || eol > headerEnd) break;
        if (eol == pos) break; // 空行
        if (!parseHeaderLine(raw.substr(pos, eol - pos))) {
            return ParseResult::ERROR;
        }
        pos = eol + 2;
    }

    // 4. 请求体长度(按 Content-Length 计算)
    size_t bodyLen = 0;
    const std::string& cl = getHeader("Content-Length");
    if (!cl.empty()) {
        for (char c : cl) {
            if (!std::isdigit(static_cast<unsigned char>(c))) {
                return ParseResult::ERROR;
            }
        }
        try {
            bodyLen = static_cast<size_t>(std::stoull(cl));
        } catch (...) {
            return ParseResult::ERROR;
        }
        if (bodyLen > MAX_BODY) {
            _errorCode = 413;
            return ParseResult::ERROR;
        }
    }

    const std::string& te = getHeader("Transfer-Encoding");
    if (!te.empty() && te != "identity") {
        _errorCode = 501; // chunked 等传输编码暂不支持
        return ParseResult::ERROR;
    }

    consumed = headerEnd + 4 + bodyLen;
    if (raw.size() < consumed) {
        return ParseResult::NEED_MORE; // 请求体尚未收全
    }
    _body = raw.substr(headerEnd + 4, bodyLen);

    // 5. Keep-Alive 判定: HTTP/1.1 默认长连接, Connection: close 显式关闭
    const std::string& conn = getHeader("Connection");
    if (_version == "HTTP/1.1") {
        _keepAlive = (conn != "close");
    } else {
        _keepAlive = (conn == "keep-alive");
    }

    return ParseResult::OK;
}

bool HttpRequest::parseRequestLine(const std::string& line)
{
    std::istringstream ss(line);
    if (!(ss >> _method >> _path >> _version)) return false;
    if (_method.empty() || _path.empty() || _version.empty()) return false;
    if (_path[0] != '/') return false;
    if (_version.compare(0, 5, "HTTP/") != 0) return false;

    // 拆分路径与查询参数
    size_t q = _path.find('?');
    if (q != std::string::npos) {
        parseQueryParams(_path.substr(q + 1));
        _path = _path.substr(0, q);
    }
    return true;
}

bool HttpRequest::parseHeaderLine(const std::string& line)
{
    size_t colon = line.find(':');
    if (colon == std::string::npos) return false;

    std::string key = line.substr(0, colon);
    std::string value = line.substr(colon + 1);

    // 去除 value 首尾空白
    size_t b = value.find_first_not_of(" \t");
    size_t e = value.find_last_not_of(" \t");
    value = (b == std::string::npos) ? "" : value.substr(b, e - b + 1);

    if (key.empty()) return false;
    _headers[key] = value;
    return true;
}

void HttpRequest::parseQueryParams(const std::string& queryStr)
{
    std::istringstream ss(queryStr);
    std::string pair;
    while (std::getline(ss, pair, '&')) {
        size_t eq = pair.find('=');
        if (eq != std::string::npos) {
            _queryParams[pair.substr(0, eq)] = pair.substr(eq + 1);
        } else if (!pair.empty()) {
            _queryParams[pair] = "";
        }
    }
}

const std::string& HttpRequest::getMethod() const { return _method; }
const std::string& HttpRequest::getPath() const { return _path; }
const std::string& HttpRequest::getVersion() const { return _version; }

const std::string& HttpRequest::getQueryParam(const std::string& key) const
{
    static const std::string emptyStr;
    auto it = _queryParams.find(key);
    return (it != _queryParams.end()) ? it->second : emptyStr;
}

const std::string& HttpRequest::getHeader(const std::string& key) const
{
    static const std::string emptyStr;
    auto it = _headers.find(key);
    return (it != _headers.end()) ? it->second : emptyStr;
}

const std::map<std::string, std::string>& HttpRequest::getAllHeaders() const
{
    return _headers;
}

const std::string& HttpRequest::getBody() const { return _body; }
bool HttpRequest::keepAlive() const { return _keepAlive; }
int HttpRequest::errorCode() const { return _errorCode; }
