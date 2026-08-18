#include "HttpResponse.h"
#include <sstream>

HttpResponse::HttpResponse()
    : _statusCode(200), _statusMessage("OK"), _version("HTTP/1.1")
{
}

HttpResponse::~HttpResponse() {}

void HttpResponse::setStatusCode(int code)
{
    _statusCode = code;
}

void HttpResponse::setStatusMessage(const std::string& message)
{
    _statusMessage = message;
}

void HttpResponse::setVersion(const std::string& version)
{
    _version = version;
}

void HttpResponse::setStatus(int code)
{
    _statusCode = code;
    switch (code) {
        case 200: _statusMessage = "OK"; break;
        case 400: _statusMessage = "Bad Request"; break;
        case 404: _statusMessage = "Not Found"; break;
        case 405: _statusMessage = "Method Not Allowed"; break;
        case 413: _statusMessage = "Payload Too Large"; break;
        case 431: _statusMessage = "Request Header Fields Too Large"; break;
        case 501: _statusMessage = "Not Implemented"; break;
        default:  _statusMessage = "Unknown"; break;
    }
}

void HttpResponse::addHeader(const std::string& key, const std::string& value)
{
    _headers[key] = value;
}

void HttpResponse::setBody(const std::string& body)
{
    _body = body;
    addHeader("Content-Length", std::to_string(_body.size()));
}

void HttpResponse::setBody(const char* body, size_t length)
{
    _body.assign(body, length);
    addHeader("Content-Length", std::to_string(_body.size()));
}

std::string HttpResponse::toString() const
{
    std::ostringstream stream;
    stream << _version << " " << _statusCode << " " << _statusMessage << "\r\n";
    for (const auto& header : _headers) {
        stream << header.first << ": " << header.second << "\r\n";
    }
    stream << "\r\n";
    stream << _body;
    return stream.str();
}

void HttpResponse::clear()
{
    _statusCode = 200;
    _statusMessage = "OK";
    _version = "HTTP/1.1";
    _headers.clear();
    _body.clear();
}
