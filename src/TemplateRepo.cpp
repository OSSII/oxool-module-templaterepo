

#include <sys/stat.h>
#include <algorithm>
#include <functional>

#include <OxOOL/Module/Base.h>
#include <OxOOL/HttpHelper.h>

#include <common/Log.hpp>
#include <net/Socket.hpp>

#include <Poco/MemoryStream.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTMLForm.h>
#include <Poco/Data/SessionPool.h>
#include <Poco/Data/Session.h>
#include <Poco/Data/RecordSet.h>
#include <Poco/Data/SQLite/Connector.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Zip/Compress.h>
#include <Poco/URI.h>
#include <Poco/TemporaryFile.h>

using namespace Poco::Data::Keywords;

class TemplateRepo : public OxOOL::Module::Base
{
public:

    enum CheckType
    {
        NONE = 0, // 不檢查
        MAC  = 1, // 檢查 Mac address
        IP   = 2  // 檢查 IP address
    };

    struct API
    {
        // request method
        std::string method;
        // 檢查類別
        CheckType check;
        // callback method
        std::function<void(const Poco::Net::HTTPRequest& request,
            const std::shared_ptr<StreamSocket>& socket)> function;
    };

    // 更新資料庫行為
    enum ActionType {ADD = 0, UPDATE, DELETE};

    struct RepositoryStruct
    {
        unsigned long id    = 0;    // AUTOINCREMENT ID
        std::string cname   = "";   // 範本資料夾名稱
        std::string endpt   = "";   // 檔案代碼(存在 server 的檔名爲 endpt + "." + extname)
        std::string docname = "";   // 實際的檔名
        std::string extname = "";   // 副檔名
        std::string uptime  = "";   // 上傳時間(比較像是檔案最後修改時間)
    };

    TemplateRepo()
    {
        // 註冊 SQLite 連結
        Poco::Data::SQLite::Connector::registerConnector();
        // 初始化 API map
        initApiMap();
    }

    ~TemplateRepo()
    {
        Poco::Data::SQLite::Connector::unregisterConnector();
    }

    void initialize() override
    {
        // 範本存放路徑
        const std::string repositoryPath = getRepositoryPath();
        // 路徑不存在就建立
        if (!Poco::File(repositoryPath).exists())
            Poco::File(repositoryPath).createDirectories();

        auto session = getDataSession();

        session << "CREATE TABLE IF NOT EXISTS maciplist ("
                << "id          INTEGER PRIMARY KEY AUTOINCREMENT,"
                << "type        TEXT NOT NULL DEFAULT '',"
                << "macip       TEXT NOT NULL DEFAULT '',"
                << "description TEXT NOT NULL DEFAULT '');"
                << "CREATE UNIQUE INDEX IF NOT EXISTS macip on maciplist(macip);", now;

        session << "CREATE TABLE IF NOT EXISTS repository ("
                << "id      INTEGER PRIMARY KEY AUTOINCREMENT,"
                << "cname   TEXT NOT NULL DEFAULT '',"
                << "endpt   TEXT NOT NULL DEFAULT '' UNIQUE,"   // end point 名稱
                << "docname TEXT NOT NULL DEFAULT '',"          // 主檔名
                << "extname TEXT NOT NULL DEFAULT '',"          // 副檔名
                << "uptime  TEXT NOT NULL DEFAULT '')", now;    // 上傳日期
    }

    void handleRequest(const Poco::Net::HTTPRequest& request,
                       const std::shared_ptr<StreamSocket>& socket) override
    {
        const std::string requestAPI = parseRealURI(request);

        // 是否支援此 API
        if (auto it = mApiMap.find(requestAPI); it != mApiMap.end())
        {
            auto api = it->second;
            // 1. 先檢查 request 方法是否正確?
            if (request.getMethod() != api.method)
            {
                std::cerr << "Accepted method is '" << api.method << "', but received is '"
                          << request.getMethod() << "'" << std::endl;
                OxOOL::HttpHelper::sendErrorAndShutdown(
                    Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED, socket);
                return;
            }

            // 2. 是否要檢查 IP or MAC address?
            switch (api.check)
            {
                case CheckType::IP:
                    // do check ip addess
                    if (!allowedIP(socket))
                    {
                        OxOOL::HttpHelper::sendErrorAndShutdown(
                            Poco::Net::HTTPResponse::HTTP_FORBIDDEN,
                            socket, "Deny access to your IP address.");
                        return;
                    }
                    break;

                case CheckType::MAC:
                    // do check mac address
                    // Mac address 是放在 client 端的 form 中
                    // 讀取 HTTML Form.
                    if (!allowedMAC(request, socket))
                    {
                        OxOOL::HttpHelper::sendErrorAndShutdown(
                            Poco::Net::HTTPResponse::HTTP_FORBIDDEN,
                            socket, "Deny access to your Mac address.");
                        return;
                    }
                    break;

                default:
                    // do nothing
                    break;
            }

            api.function(request, socket); // 執行對應的 API
        }
        else // 沒有相對應的 API 就回應 NOT FOUND
        {
            std::cerr << "unknow api : " << requestAPI << "\n";
            OxOOL::HttpHelper::sendErrorAndShutdown(
                Poco::Net::HTTPResponse::HTTP_NOT_FOUND, socket);
        }
    }

    //
    std::string handleAdminMessage(const StringVector& tokens) override
    {
        auto session = getDataSession();
        // 取得 Mac 及 IP 列表
        if (tokens.equals(0, "getList"))
        {
            Poco::JSON::Object json;

            static std::map<std::string, std::string> mapList = {{"mac", "macList"}, {"ip", "ipList"}};
            for (auto it : mapList)
            {
                std::string type = it.first;
                std::string group = it.second;

                Poco::JSON::Array array;

                std::vector<Poco::Tuple<unsigned int, std::string, std::string>> records;

                session << "SELECT id, macip, description FROM maciplist WHERE type=?",
                        use(type), into(records), now;

                for (auto record : records)
                {
                    Poco::JSON::Object property;
                    property.set("id", record.get<0>());
                    property.set("value", record.get<1>());
                    property.set("desc", record.get<2>());
                    array.add(property);
                }
                json.set(group, array);
            }

            std::ostringstream oss;
            json.stringify(oss);
            return "macipList " + oss.str();
        }
        // 新增來源
        else if (tokens.equals(0, "addSource") && tokens.size() == 3)
        {
            std::string jsonStr;
            Poco::URI::decode(tokens[2], jsonStr);

            Poco::JSON::Parser parser;
            auto result = parser.parse(jsonStr);
            try
            {
                Poco::JSON::Object::Ptr json = result.extract<Poco::JSON::Object::Ptr>();
                std::string type = tokens[1];
                std::string macip = json->getValue<std::string>("value");
                std::string description = json->getValue<std::string>("desc");
                unsigned long lastId = 0;

                // 新增紀錄
                session << "INSERT INTO maciplist (type, macip, description) "
                        << "VALUES(?, ?, ?)", use(type), use(macip), use(description), now;
                // 取得剛剛新增的 id 編號
                session << "SELECT last_insert_rowid()", into(lastId), now;

                json->set("id", lastId);
                const std::string cmd = (type == "mac" ? "addMacList" : "addIpList");
                std::ostringstream oss;
                json->stringify(oss);
                return cmd + " " + oss.str();

            }
            catch(const Poco::Exception& exc)
            {
                LOG_ERR("Admin module [" << getDetail().name << "]:" << exc.displayText());
                return "Error:" + exc.displayText();
            }
        }
        // 更新來源
        else if (tokens.equals(0, "updateSource") && tokens.size() == 2)
        {
            std::string jsonStr;
            Poco::URI::decode(tokens[1], jsonStr);

            Poco::JSON::Parser parser;
            auto result = parser.parse(jsonStr);
            try
            {
                Poco::JSON::Object::Ptr json = result.extract<Poco::JSON::Object::Ptr>();
                unsigned long id = json->getValue<unsigned long>("id");
                std::string macip = json->getValue<std::string>("value");
                std::string description = json->getValue<std::string>("desc");

                session << "UPDATE maciplist SET macip=?, description=? "
                        << "WHERE id=?", use(macip), use(description), use(id), now;

                std::ostringstream oss;
                json->stringify(oss);
                return "updateSource " + oss.str();
            }
            catch(const Poco::Exception& exc)
            {
                LOG_ERR("Admin module [" << getDetail().name << "]:" << exc.displayText());
                return "Error:" + exc.displayText();
            }

        }
        // 刪除來源
        else if (tokens.equals(0, "deleteSource") && tokens.size() == 2)
        {
            unsigned long id = std::stoul(tokens[1]);
            try
            {
                session << "DELETE FROM maciplist WHERE id=?", use(id), now;

                return "deleteSource " + tokens[1];
            }
            catch(const Poco::Exception& exc)
            {
                LOG_ERR("Admin module [" << getDetail().name << "]:" << exc.displayText());
                return "Error:" + exc.displayText();
            }
        }

        return "";
    }

private:
    std::map<std::string, API> mApiMap;

    /// @brief 檢查 IP 來源是否允許
    /// @param socket
    /// @return true - 允許
    bool allowedIP(const std::shared_ptr<StreamSocket>& socket)
    {
        bool allowed = false; // 預設不允許

        std::string clientAddress = socket->clientAddress();

        // 是不是 IPV4
        bool isIPv4 = clientAddress.find_first_of("::ffff:") == 0;
        if (isIPv4)
            clientAddress = clientAddress.substr(7);

        if (clientAddress == "::1" || clientAddress == "127.0.0.1")
            allowed = true;
        else
        {
            unsigned long count = 0;
            auto session = getDataSession();
            session << "SELECT COUNT(macip) FROM maciplist "
                    << "WHERE type='ip' AND macip=?",
                    into(count), use(clientAddress), now;
            allowed = (count == 1);
        }

        return allowed;
    }

    bool allowedMAC(const Poco::Net::HTTPRequest& request,
                    const std::shared_ptr<StreamSocket>& socket)
    {
        // 讀取 HTTML Form.
        Poco::MemoryInputStream message(&socket->getInBuffer()[0],
                                        socket->getInBuffer().size());
        const Poco::Net::HTMLForm form(request, message);
        std::string macAddress = form.get("mac_addr", "");

        if (macAddress.empty())
            return false;

        // 轉小寫
        std::transform(macAddress.begin(), macAddress.end(), macAddress.begin(),
            [](unsigned char c){ return std::tolower(c); });

        unsigned long count = 0;
        auto session = getDataSession();
        session << "SELECT COUNT(macip) FROM maciplist "
                << "WHERE type='mac' AND macip=?",
                into(count), use(macAddress), now;

        return (count == 1);
    }

    void initApiMap()
    {
        mApiMap =
        {
            /* {
                "/api",
                {
                    method: Poco::Net::HTTPRequest::HTTP_GET,
                    check: CheckType::IP,
                    function: std::bind(&TemplateRepo::yamlAPI, this,
                        std::placeholders::_1, std::placeholders::_2)
                }
            }, */
            {
                "/list",
                {
                    method: Poco::Net::HTTPRequest::HTTP_GET,
                    check: CheckType::NONE,
                    function: std::bind(&TemplateRepo::listAPI, this,
                        std::placeholders::_1, std::placeholders::_2)
                }
            },
            {
                "/sync",
                {
                    method: Poco::Net::HTTPRequest::HTTP_POST,
                    check: CheckType::MAC,
                    function: std::bind(&TemplateRepo::syncAPI, this,
                        std::placeholders::_1, std::placeholders::_2)
                }
            },
            {
                "/upload",
                {
                    method: Poco::Net::HTTPRequest::HTTP_POST,
                    check: CheckType::IP,
                    function: std::bind(&TemplateRepo::uploadAPI, this,
                        std::placeholders::_1, std::placeholders::_2)
                }
            },
            {
                "/update",
                {
                    method: Poco::Net::HTTPRequest::HTTP_POST,
                    check: CheckType::IP,
                    function: std::bind(&TemplateRepo::updateAPI, this,
                        std::placeholders::_1, std::placeholders::_2)
                }
            },
            {
                "/delete",
                {
                    method: Poco::Net::HTTPRequest::HTTP_POST,
                    check: CheckType::IP,
                    function: std::bind(&TemplateRepo::deleteAPI, this,
                        std::placeholders::_1, std::placeholders::_2)
                }
            },
            {
                "/download",
                {
                    method: Poco::Net::HTTPRequest::HTTP_POST,
                    check: CheckType::MAC,
                    function: std::bind(&TemplateRepo::downloadAPI, this,
                        std::placeholders::_1, std::placeholders::_2)
                }
            }
        };
    }

    /* /// @brief
    void yamlAPI(const Poco::Net::HTTPRequest& request,
                 const std::shared_ptr<StreamSocket>& socket)
    {
        std::string yaml = R"(
        swagger: '2.0'
        info:
          version: v1
          title: ODF Template Center API
          description: ''
        host: '${HOST}'
        paths:
          ${SERVICE_URI}list:
            get:
              responses:
                '200':
                  description: Success
                  schema:
                    type: object
                    properties:
                      Category:
                        type: array
                        items:
                          $ref: '#/definitions/Category'
          ${SERVICE_URI}sync:
            post:
              consumes:
                - application/json
              parameters:
                - $ref : '#/parameters/Sync'
              responses:
                '200':
                  description: Success
                '401':
                  description: Json data error
        schemes:
          - http
          - https
        definitions:
          Category:
            type: object
            required:
              - uptime
              - endpt
              - cid
              - hide
              - extname
              - docname
            properties:
              uptime:
                type: string
                format: date-time
              endpt:
                type: string
              cid:
                type: string
              hide:
                type: string
              extname:
                type: string
              docname:
                type: string

        parameters:
          Sync:
            in: body
            name: body
            description: ''
            required: true
            schema:
              type: object
              properties:
                Category:
                  type: array
                  items:
                    $ref: '#/definitions/Category'
        )";

        Poco::replaceInPlace(yaml, std::string("${SERVICE_URI}"), getDetail().serviceURI);
        Poco::replaceInPlace(yaml, std::string("${HOST}"), request.getHost());

        OxOOL::HttpHelper::sendResponseAndShutdown(socket, yaml,
            Poco::Net::HTTPResponse::HTTP_OK, "text/yaml; charset=utf-8");
    } */

    void listAPI(const Poco::Net::HTTPRequest& /*request*/,
                 const std::shared_ptr<StreamSocket>& socket)
    {
         auto session = getDataSession();

        // 查詢範本類別列表
        std::vector<std::string> groups;
        session << "SELECT cname FROM repository GROUP BY cname", into(groups), now;

        // 依據範本類別分組
        Poco::JSON::Object json;
        for (auto group : groups)
        {
            Poco::JSON::Array groupArray;
            // 查詢各組明細
            Poco::Data::Statement select(session);
            select << "SELECT docname, endpt, extname, uptime FROM repository WHERE cname=?", use(group), now;
            Poco::Data::RecordSet rs(select);

            std::size_t cols = rs.columnCount(); // 取欄位數
            // 遍歷所有資料列
            for (auto row : rs)
            {
                // 轉為 JSON 物件
                Poco::JSON::Object obj;
                for (std::size_t col = 0; col < cols ; col++)
                {
                    obj.set(rs.columnName(col), row.get(col));
                }
                // 加入該組陣列
                groupArray.add(obj);
            }

            // 完成一組
            json.set(group, groupArray);
        }

        std::ostringstream oss;
        json.stringify(oss, 4);
        OxOOL::HttpHelper::sendResponseAndShutdown(socket, oss.str());
    }

    void syncAPI(const Poco::Net::HTTPRequest& request,
                 const std::shared_ptr<StreamSocket>& socket)
    {
        // 讀取 HTTML Form.
        Poco::MemoryInputStream message(&socket->getInBuffer()[0],
                                        socket->getInBuffer().size());
        const Poco::Net::HTMLForm form(request, message);

        const std::string jsonStr = form.get("data", "{}");

        bool syntaxError = false;

        // 製作暫存路徑
        const Poco::Path tmpPath = Poco::Path::forDirectory(Poco::TemporaryFile::tempName());
        Poco::File(tmpPath).createDirectories();
        chmod(tmpPath.toString().c_str(), S_IXUSR | S_IWUSR | S_IRUSR);

        try
        {
            Poco::JSON::Parser parser;
            auto result = parser.parse(jsonStr);
            const Poco::JSON::Object::Ptr json = result.extract<Poco::JSON::Object::Ptr>();

            // json 結構檢查
            for (auto it = json->begin(); it != json->end() ; ++it)
            {
                // first is key
                const std::string group = it->first;
                // second is array
                if (!it->second.isArray())
                {
                    syntaxError = true;
                    break;
                }
                const Poco::Path groupPath = Poco::Path::forDirectory(tmpPath.toString() + group);
                Poco::File(groupPath).createDirectory(); // 建立羣組目錄

                Poco::JSON::Array::Ptr array = it->second.extract<Poco::JSON::Array::Ptr>();
                for (auto obj = array->begin(); obj != array->end(); ++obj)
                {
                    Poco::JSON::Object::Ptr object = obj->extract<Poco::JSON::Object::Ptr>();
                    // 利用 endpt 取得原始記錄
                    std::string endpt = object->getValue<std::string>("endpt");
                    RepositoryStruct repo = getRepository(endpt);

                    // 原始檔案
                    Poco::File sourceFile(getRepositoryPath() + "/" + repo.endpt + "." + repo.extname);
                    const std::string destFile = repo.docname + "." + repo.extname;
                    // 檔案存在就複製
                    if (sourceFile.exists())
                    {
                        // 複製到群組目錄下
                        sourceFile.copyTo(groupPath.toString() + destFile);
                    }
                }
            }
        }
        catch(const Poco::Exception& exc)
        {
            LOG_ERR("Admin module [" << getDetail().name << "]:" << exc.displayText());
            syntaxError = true;
        }

        if (syntaxError)
        {
            OxOOL::HttpHelper::sendErrorAndShutdown(Poco::Net::HTTPResponse::HTTP_BAD_REQUEST,
                socket, "Request data syntax error.");
        }
        else
        {
            // 壓縮成 zip 檔案
            const std::string zipFile = Poco::TemporaryFile::tempName() + ".zip";

            std::ofstream zipOut(zipFile, std::ios::binary);
            Poco::Zip::Compress compress(zipOut, true);
            compress.addRecursive(tmpPath, Poco::Zip::ZipCommon::CL_NORMAL);
            compress.close();

            // 傳回檔案
            Poco::Net::HTTPResponse response;
            response.set("Content-Disposition", "attachment; filename=\"" + zipFile + "\"");
            OxOOL::HttpHelper::sendFileAndShutdown(socket, zipFile,
                "application/octet-stream", &response, true);
            // 移除檔案
            Poco::File(zipFile).remove();
        }

        // 徹底移除暫存目錄
        Poco::File(tmpPath).remove(true);
    }

    void uploadAPI(const Poco::Net::HTTPRequest& request,
                   const std::shared_ptr<StreamSocket>& socket)
    {
        // 讀取 HTTML Form.
        OxOOL::HttpHelper::PartHandler partHandler;
        Poco::MemoryInputStream message(&socket->getInBuffer()[0],
                                        socket->getInBuffer().size());
        const Poco::Net::HTMLForm form(request, message, partHandler);

        // 從 form 取值
        RepositoryStruct repo =
        {
            cname:   form.get("cname", ""),
            endpt:   form.get("endpt", ""),
            docname: form.get("docname", ""),
            extname: form.get("extname", ""),
            uptime:  form.get("uptime", "")
        };

        // 有收到檔案
        if (!partHandler.empty())
        {
            const Poco::File recivedFile(partHandler.getFilename());
            const std::string newName = getRepositoryPath() + "/"
                                      + form.get("endpt") + "." + form.get("extname");
            // 收到的檔案複製一份並改名，存到 RepositoryPath 路徑下
            recivedFile.copyTo(newName);
            // 移除收到的檔案
            partHandler.removeFiles();

            // 更新資料庫(新增)
            updateRepositoryData(ActionType::ADD, repo);

            OxOOL::HttpHelper::sendResponseAndShutdown(socket, "Upload Success.");
        }
        else // 沒有收到檔案
        {
            OxOOL::HttpHelper::sendErrorAndShutdown(
                Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, socket, "File not received.");
        }
    }

    void updateAPI(const Poco::Net::HTTPRequest& request,
                   const std::shared_ptr<StreamSocket>& socket)
    {
        // 讀取 HTTML Form.
        OxOOL::HttpHelper::PartHandler partHandler;
        Poco::MemoryInputStream message(&socket->getInBuffer()[0],
                                        socket->getInBuffer().size());
        const Poco::Net::HTMLForm form(request, message, partHandler);

        // 有收到檔案
        if (!partHandler.empty())
        {
            // 收到的檔案
            const Poco::File recivedFile(partHandler.getFilename());
            std::string endpt = form.get("endpt", "");
            // 讀取該筆原始記錄
            RepositoryStruct repo = getRepository(endpt);
            // 確實有資料
            if (repo.id != 0)
            {
                Poco::File oldFile(getRepositoryPath() + "/" + repo.endpt + "." + repo.extname);
                // 舊檔案存在就刪除它
                if (oldFile.exists())
                {
                    oldFile.remove();
                }
                // 更新資料庫(刪除)
                updateRepositoryData(ActionType::DELETE, repo);
            }

            // 紀錄新資料
            repo.endpt   = endpt;
            repo.extname = form.get("extname", "");
            repo.uptime  = form.get("uptime", "");
            // 新的檔名應該要一樣
            const std::string newName = getRepositoryPath() + "/"
                                        + repo.endpt + "." + repo.extname;
            // 收到的檔案複製一份並改名，存到 RepositoryPath 路徑下
            recivedFile.copyTo(newName);

            // 更新資料庫(新增)
            updateRepositoryData(ActionType::ADD, repo);

            // 移除收到的檔案
            partHandler.removeFiles();

            OxOOL::HttpHelper::sendResponseAndShutdown(socket, "Update Success.");
        }
        else // 沒有收到檔案
        {
            OxOOL::HttpHelper::sendErrorAndShutdown(
                Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, socket, "File not received.");
        }
    }

    void deleteAPI(const Poco::Net::HTTPRequest& request,
                   const std::shared_ptr<StreamSocket>& socket)
    {
        // 讀取 HTTML Form.
        Poco::MemoryInputStream message(&socket->getInBuffer()[0],
                                        socket->getInBuffer().size());
        const Poco::Net::HTMLForm form(request, message);

        // 從 form 取值
        RepositoryStruct repo =
        {
            endpt: form.get("endpt", ""),
            extname: form.get("extname", "")
        };

        if (repo.endpt.empty())
        {
            OxOOL::HttpHelper::sendErrorAndShutdown(
                Poco::Net::HTTPResponse::HTTP_BAD_REQUEST, socket, "No endpt provide.");
        }
        else
        {
            Poco::File targetFile(getRepositoryPath() + "/" + repo.endpt + "." + repo.extname);
            // 指定檔案存在
            if (targetFile.exists())
            {
                targetFile.remove(); // 刪除指定檔案
                // 更新資料庫(刪除)
                updateRepositoryData(ActionType::DELETE, repo);
                OxOOL::HttpHelper::sendResponseAndShutdown(socket, "Delete success.");
            }
            else
            {
                OxOOL::HttpHelper::sendErrorAndShutdown(
                    Poco::Net::HTTPResponse::HTTP_NOT_FOUND, socket,
                    "The file to be deleted does not exist");
            }
        }
    }

    void downloadAPI(const Poco::Net::HTTPRequest& request,
                     const std::shared_ptr<StreamSocket>& socket)
    {
        // 讀取 HTTML Form.
        Poco::MemoryInputStream message(&socket->getInBuffer()[0],
                                        socket->getInBuffer().size());
        const Poco::Net::HTMLForm form(request, message);

        // 讀取紀錄
        std::string endpt = form.get("endpt", "");
        RepositoryStruct repo = getRepository(endpt);
        // 有記錄
        if (repo.id != 0)
        {
            std::string requestFile = getRepositoryPath() + "/" + repo.endpt + "." + repo.extname;
            // 檔案存在
            if (Poco::File(requestFile).exists())
            {
                const std::string fileName = repo.docname + "." + repo.extname;

                Poco::Net::HTTPResponse response;
                response.set("Content-Disposition", "attachment; filename=\"" + fileName + '"');

                OxOOL::HttpHelper::sendFileAndShutdown(socket, requestFile,
                    "application/octet-stream", &response, true);
                return;
            }
        }
        OxOOL::HttpHelper::sendErrorAndShutdown(Poco::Net::HTTPResponse::HTTP_NOT_FOUND, socket);
    }

// 處理資料庫相關的 methods
private:
    /// @brief 取得可用的 data session
    /// @return Poco::Data::Session
    Poco::Data::Session getDataSession()
    {
        static std::string dbName = getDocumentRoot() + "/data.db";
        static Poco::Data::SessionPool sessionPool("SQLite", dbName);
        return sessionPool.get();
    }

    /// @brief 取得符合 endpt 的紀錄
    /// @param endpt
    /// @return RepositoryStruct
    RepositoryStruct getRepository(std::string& endpt)
    {
        RepositoryStruct repo;

        auto session = getDataSession();
        try
        {
            session << "SELECT id, cname, docname, endpt, extname, uptime FROM repository WHERE endpt=?",
                into(repo.id), into(repo.cname), into(repo.docname),
                into(repo.endpt), into(repo.extname), into(repo.uptime),
                use(endpt), now;
        }
        catch(const Poco::Exception& exc)
        {
            std::cerr << "Admin module [" << getDetail().name << "] update database:"
                      << exc.displayText() << std::endl;
        }

        return repo;
    }

    /// @brief 更新範本資料表
    /// @param RepositoryStruc
    /// @return
    bool updateRepositoryData(ActionType type, RepositoryStruct& repo)
    {
        try
        {
            auto session = getDataSession();
            switch (type)
            {
                case ActionType::ADD: // 新增
                    session << "INSERT INTO repository (endpt, extname, cname, docname, uptime) "
                            << "VALUES(?, ?, ?, ?, ?)",
                            use(repo.endpt), use(repo.extname),
                            use(repo.cname), use(repo.docname),
                            use(repo.uptime), now;
                    break;

                case ActionType::UPDATE: // 更新
                    break;

                case ActionType::DELETE: // 刪除
                    session << "DELETE FROM repository WHERE endpt=?", use(repo.endpt), now;
                    break;
            }
        }
        catch(const Poco::Exception& exc)
        {
            LOG_ERR("Admin module [" << getDetail().name << "] update database:" << exc.displayText());
            return false;
        }

        return true;
    }

private:
    /// @brief 取得範本倉庫路徑
    const std::string& getRepositoryPath()
    {
        static std::string repositoryPath = getDocumentRoot() + "/repository";
        return repositoryPath;
    }
};

OXOOL_MODULE_EXPORT(TemplateRepo);
