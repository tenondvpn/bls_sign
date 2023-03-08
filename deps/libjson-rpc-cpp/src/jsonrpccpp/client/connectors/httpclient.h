/*************************************************************************
 * libjson-rpc-cpp
 *************************************************************************
 * @file    httpclient.h
 * @date    02.01.2013
 * @author  Peter Spiess-Knafl <dev@spiessknafl.at>
 * @license See attached LICENSE.txt
 ************************************************************************/

#ifndef JSONRPC_CPP_HTTPCLIENT_H_
#define JSONRPC_CPP_HTTPCLIENT_H_

#include "../iclientconnector.h"
#include <curl/curl.h>
#include <jsonrpccpp/common/exception.h>
#include <map>

namespace jsonrpc {




class HttpClient : public IClientConnector {

    static std::string  certFileFullPath;
    static std::string keyFileFullPath;
    static uint32_t sslClientPort;
public:
    static uint32_t getSslClientPort();

    static void setSslClientPort(uint32_t sslClientPort);

public:
  HttpClient(const std::string &url);
  virtual ~HttpClient();
  virtual void SendRPCMessage(const std::string &message, std::string &result);

  void SetUrl(const std::string &url);
  void SetTimeout(long timeout);

  void AddHeader(const std::string &attr, const std::string &val);
  void RemoveHeader(const std::string &attr);

    static const std::string &getCertFileFullPath();

    static void setCertFileFullPath(const std::string &certFileFullPath);

    static const std::string &getKeyFileFullPath();

    static void setKeyFileFullPath(const std::string &keyFileFullPath);

private:
  std::map<std::string, std::string> headers;
  std::string url;

  /**
   * @brief timeout for http request in milliseconds
   */
  long timeout;
  CURL *curl;
};

} /* namespace jsonrpc */
#endif /* JSONRPC_CPP_HTTPCLIENT_H_ */
