#include "Server.h"

#include <set>

#include "Common/Types.h"
#include "Common/GstRtspServerPtr.h"

#include "Log.h"
#include "StaticSources.h"
#include "RtspMountPoints.h"


namespace RestreamServer
{

namespace
{

struct ClientInfo
{
    std::set<std::string> refPaths;
};

struct PathInfo
{
    std::set<const GstRTSPClient*> refClients;
    unsigned playCount;
    const GstRTSPClient* recordClient;
    std::string recordSessionId;
};

}

struct Server::Private
{
    GstRTSPServerPtr staticServer;

    GstRTSPServerPtr restreamServer;
    GstRTSPMountPointsPtr mountPoints;

    std::map<const GstRTSPClient*, ClientInfo> clients;
    std::map<std::string, PathInfo> paths;

    PathInfo& registerPath(const GstRTSPClient*, const std::string& path);

    void onClientConnected(GstRTSPClient*);

    GstRTSPStatusCode beforePlay(const GstRTSPClient*, const GstRTSPUrl*, const gchar* sessionId);
    void onPlay(const GstRTSPClient*, const GstRTSPUrl*, const gchar* sessionId);
    GstRTSPStatusCode beforeRecord(const GstRTSPClient*, const GstRTSPUrl*, const gchar* sessionId);
    void onRecord(const GstRTSPClient*, const GstRTSPUrl*, const gchar* sessionId);
    void onTeardown(const GstRTSPClient*, const GstRTSPUrl*, const gchar* sessionId);

    void onClientClosed(const GstRTSPClient*);

    void firstPlayerConnected(const PathInfo&) {}
    void lastPlayerDisconnected(const PathInfo&) {}

    void recorderConnected(const PathInfo&) {}
    void recorderDisconnected(const PathInfo&) {}
};


const std::shared_ptr<spdlog::logger>& Server::Log()
{
    return RestreamServer::Log();
}

PathInfo& Server::Private::registerPath(const GstRTSPClient* client, const std::string& path)
{
    const auto clientIt = clients.find(client);
    if(clients.end() == clientIt) {
        clients.emplace(client, ClientInfo{.refPaths = {path}});
    } else {
        auto& refPaths = clientIt->second.refPaths;
        refPaths.emplace(path);
    }

    auto pathIt = paths.find(path);
    if(paths.end() == pathIt) {
        pathIt =
            paths.emplace(
                path,
                PathInfo {
                .refClients = {client},
                .playCount = 0,
                .recordClient = nullptr}).first;
    } else {
        auto& refClients = pathIt->second.refClients;
        refClients.emplace(client);
    }

    return pathIt->second;
}

void Server::Private::onClientConnected(GstRTSPClient* client)
{
    GstRTSPConnection* connection =
        gst_rtsp_client_get_connection(client);
    Log()->info(
        "New connection from {}",
        gst_rtsp_connection_get_ip(connection));

    auto prePlayCallback =
        (GstRTSPStatusCode (*)(GstRTSPClient*, GstRTSPContext*, gpointer))
        [] (GstRTSPClient* client, GstRTSPContext* context, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            const gchar* sessionId =
                gst_rtsp_session_get_sessionid(context->session);
            return p->beforePlay(client, context->uri, sessionId);
        };
    g_signal_connect(client, "pre-play-request", GCallback(prePlayCallback), this);

    auto playCallback = (void (*)(GstRTSPClient*, GstRTSPContext*, gpointer))
        [] (GstRTSPClient* client, GstRTSPContext* context, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            const gchar* sessionId =
                gst_rtsp_session_get_sessionid(context->session);
            p->onPlay(client, context->uri, sessionId);
        };
    g_signal_connect(client, "play-request", GCallback(playCallback), this);

    auto preRecordCallback =
        (GstRTSPStatusCode (*)(GstRTSPClient*, GstRTSPContext*, gpointer))
        [] (GstRTSPClient* client, GstRTSPContext* context, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            const gchar* sessionId =
                gst_rtsp_session_get_sessionid(context->session);
            return p->beforeRecord(client, context->uri, sessionId);
        };
    g_signal_connect(client, "pre-record-request", GCallback(preRecordCallback), this);

    auto recordCallback = (void (*)(GstRTSPClient*, GstRTSPContext*, gpointer))
        [] (GstRTSPClient* client, GstRTSPContext* context, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            const gchar* sessionId =
                gst_rtsp_session_get_sessionid(context->session);
            p->onRecord(client, context->uri, sessionId);
        };
    g_signal_connect(client, "record-request", GCallback(recordCallback), this);

    auto teardownCallback = (void (*)(GstRTSPClient*, GstRTSPContext*, gpointer))
        [] (GstRTSPClient* client, GstRTSPContext* context, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            const gchar* sessionId =
                gst_rtsp_session_get_sessionid(context->session);
            p->onTeardown(client, context->uri, sessionId);
        };
    g_signal_connect(client, "teardown-request", GCallback(teardownCallback), this);

    auto closedCallback= (void (*)(GstRTSPClient*, gpointer))
        [] (GstRTSPClient* client, gpointer userData) {
            Private* p =
                static_cast<Private*>(userData);
            p->onClientClosed(client);
        };
    g_signal_connect(client, "closed", GCallback(closedCallback), this);
}

GstRTSPStatusCode Server::Private::beforePlay(
    const GstRTSPClient* client,
    const GstRTSPUrl* url,
    const gchar* sessionId)
{
    Log()->debug(
        "Server.beforePlay."
        " client: {}, path: {}, sessionId: {}",
        static_cast<const void*>(client), url->abspath, sessionId);

    const std::string path = url->abspath;

    auto pathIt = paths.find(path);
    if(MAX_CLIENTS_PER_PATH > 0 && paths.end() != pathIt) {
        if(pathIt->second.playCount >= (MAX_CLIENTS_PER_PATH - 1)) {
            Log()->error(
                "Max players count limit reached. "
                "client: {}, path: {}, sessionId: {}",
                static_cast<const void*>(client), url->abspath, sessionId);
            return GST_RTSP_STS_FORBIDDEN;
        }
    }

    return GST_RTSP_STS_OK;
}

void Server::Private::onPlay(
    const GstRTSPClient* client,
    const GstRTSPUrl* url,
    const gchar* sessionId)
{
    Log()->debug(
        "Server.play."
        " client: {}, path: {}, sessionId: {}",
        static_cast<const void*>(client), url->abspath, sessionId);

    const std::string path = url->abspath;

    PathInfo& pathInfo = registerPath(client, path);
    ++pathInfo.playCount;
    if(1 == pathInfo.playCount)
        firstPlayerConnected(pathInfo);
}

GstRTSPStatusCode Server::Private::beforeRecord(
    const GstRTSPClient* client,
    const GstRTSPUrl* url,
    const gchar* sessionId)
{
    Log()->debug(
        "Server.beforeRecord."
        " client: {}, path: {}, sessionId: {}",
        static_cast<const void*>(client), url->abspath, sessionId);

    return GST_RTSP_STS_OK;
}

void Server::Private::onRecord(
    const GstRTSPClient* client,
    const GstRTSPUrl* url,
    const gchar* sessionId)
{
    Log()->debug(
        "Server.record. "
        "client: {}, path: {}, sessionId: {}",
        static_cast<const void*>(client), url->abspath, sessionId);

    const std::string path = url->abspath;

    PathInfo& pathInfo = registerPath(client, path);
    if(pathInfo.recordClient || !pathInfo.recordSessionId.empty()) {
        Log()->critical(
            "Second record on the same path. client: {}, path: {}",
            static_cast<const void*>(client), url->abspath);
    } else {
        pathInfo.recordClient = client;
        pathInfo.recordSessionId = sessionId;

        recorderConnected(pathInfo);
    }
}

void Server::Private::onTeardown(
    const GstRTSPClient* client,
    const GstRTSPUrl* url,
    const gchar* sessionId)
{
    Log()->debug(
        "Server.teardown. "
        "client: {}, path: {}, sessionId: {}",
        static_cast<const void*>(client), url->abspath, sessionId);

    const std::string path = url->abspath;

    auto pathIt = paths.find(path);
    if(paths.end() == pathIt) {
        Log()->critical(
            "Not registered path teardown. client: {}, path: {}",
            static_cast<const void*>(client), url->abspath);
        return;
    }

    PathInfo& pathInfo = pathIt->second;
    if(client == pathInfo.recordClient &&
       sessionId == pathInfo.recordSessionId)
    {
        pathInfo.recordClient = nullptr;
        pathInfo.recordSessionId.clear();

        recorderDisconnected(pathInfo);
    } else {
        if(pathInfo.playCount > 0) {
            --pathInfo.playCount;
            if(0 == pathInfo.playCount)
                lastPlayerDisconnected(pathInfo);
        } else {
            Log()->critical(
                "Not registered reader teardown. client: {}, path: {}",
                static_cast<const void*>(client), url->abspath);
        }
    }
}

void Server::Private::onClientClosed(const GstRTSPClient* client)
{
    Log()->debug(
        "Server.clientClosed. "
        "client: {}",
        static_cast<const void*>(client));

    const auto clientIt = clients.find(client);
    if(clients.end() != clientIt) {
        auto& refPaths = clientIt->second.refPaths;
        for(const std::string& path: refPaths) {
            auto pathIt = paths.find(path);
            if(paths.end() != pathIt) {
                PathInfo& pathInfo = pathIt->second;

                auto& refClients = pathIt->second.refClients;
                refClients.erase(client);

                if(refClients.empty()) {
                    // last client was reader
                    if(nullptr == pathInfo.recordClient)
                        lastPlayerDisconnected(pathInfo);
                    else {
                        assert(pathInfo.recordClient == client);

                        pathInfo.recordClient = nullptr;
                        pathInfo.recordSessionId.clear();

                        recorderDisconnected(pathInfo);
                    }

                    paths.erase(pathIt);
                } else {
                    if(client == pathInfo.recordClient) {
                        pathInfo.recordClient = nullptr;
                        pathInfo.recordSessionId.clear();

                        recorderDisconnected(pathInfo);
                    }

                    if(++refClients.begin() == refClients.end()) {
                        // if only writer remained
                        if(nullptr != pathInfo.recordClient)
                            lastPlayerDisconnected(pathInfo);
                    }
                }
            } else {
                Log()->critical("Inconsitency between clients and paths");
            }
        }
        clients.erase(clientIt);
    }
}


Server::Server() :
    _p(new Private())
{
    gst_init(0, nullptr);

    initStaticServer();
    initRestreamServer();
}

Server::~Server()
{
    _p.reset();
}

void Server::initStaticServer()
{
    _p->staticServer.reset(gst_rtsp_server_new());
    GstRTSPMountPointsPtr mountPointsPtr(gst_rtsp_mount_points_new());

    GstRTSPServer* server = _p->staticServer.get();
    GstRTSPMountPoints* mountPoints = mountPointsPtr.get();

    gst_rtsp_server_set_service(server, STATIC_SERVER_PORT_STR);

    gst_rtsp_server_set_mount_points(server, mountPoints);

    {
        GstRTSPMediaFactory* barsFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            barsFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(barsFactory,
            "( videotestsrc pattern=smpte100 ! x264enc ! rtph264pay name=pay0 pt=96 )");
        gst_rtsp_media_factory_set_shared(barsFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, BARS, barsFactory);
    }

    {
        GstRTSPMediaFactory* whiteScreenFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            whiteScreenFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(whiteScreenFactory,
            "( videotestsrc pattern=white ! x264enc ! rtph264pay name=pay0 pt=96 )");
        gst_rtsp_media_factory_set_shared(whiteScreenFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, WHITE, whiteScreenFactory);
    }

    {
        GstRTSPMediaFactory* blackScreenFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            blackScreenFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(blackScreenFactory,
            "( videotestsrc pattern=black ! x264enc ! rtph264pay name=pay0 pt=96 )");
        gst_rtsp_media_factory_set_shared(blackScreenFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, BLACK, blackScreenFactory);
    }

    {
        GstRTSPMediaFactory* redScreenFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            redScreenFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(redScreenFactory,
            "( videotestsrc pattern=red ! x264enc ! rtph264pay name=pay0 pt=96 )");
        gst_rtsp_media_factory_set_shared(redScreenFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, RED, redScreenFactory);
    }

    {
        GstRTSPMediaFactory* greenScreenFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            greenScreenFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(greenScreenFactory,
            "( videotestsrc pattern=green ! x264enc ! rtph264pay name=pay0 pt=96 )");
        gst_rtsp_media_factory_set_shared(greenScreenFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, GREEN, greenScreenFactory);
    }

    {
        GstRTSPMediaFactory* blueScreenFactory = gst_rtsp_media_factory_new();
        gst_rtsp_media_factory_set_transport_mode(
            blueScreenFactory, GST_RTSP_TRANSPORT_MODE_PLAY);
        gst_rtsp_media_factory_set_launch(blueScreenFactory,
            "( videotestsrc pattern=blue ! x264enc ! rtph264pay name=pay0 pt=96 )");
        gst_rtsp_media_factory_set_shared(blueScreenFactory, TRUE);
        gst_rtsp_mount_points_add_factory(mountPoints, BLUE, blueScreenFactory);
    }
}

void Server::initRestreamServer()
{
    _p->restreamServer.reset(gst_rtsp_server_new());
    _p->mountPoints.reset(GST_RTSP_MOUNT_POINTS(rtsp_mount_points_new()));

    GstRTSPServer* server = _p->restreamServer.get();
    GstRTSPMountPoints* mountPoints = _p->mountPoints.get();

    gst_rtsp_server_set_service(server, RESTREAM_SERVER_PORT_STR);

    gst_rtsp_server_set_mount_points(server, mountPoints);

    auto clientConnectedCallback =
        (void (*)(GstRTSPServer*, GstRTSPClient*, gpointer))
        [] (GstRTSPServer* /*server*/,
            GstRTSPClient* client,
            gpointer userData)
        {
            Private* p =
                static_cast<Private*>(userData);
            p->onClientConnected(client);
        };
    g_signal_connect(
        server, "client-connected",
        (GCallback) clientConnectedCallback, _p.get());
}

void Server::serverMain()
{
    GstRTSPServer* staticServer = _p->staticServer.get();
    GstRTSPServer* restreamServer = _p->restreamServer.get();

    if(!staticServer) {
        Log()->critical("RTSP static server not initialized");
        return;
    }
    if(!restreamServer) {
        Log()->critical("RTSP restream server not initialized");
        return;
    }

    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);

    gst_rtsp_server_attach(staticServer, nullptr);
    gst_rtsp_server_attach(restreamServer, nullptr);

    Log()->info(
        "RTSP static server running on port {}",
        gst_rtsp_server_get_bound_port(staticServer));
    Log()->info(
        "RTSP restream server running on port {}",
        gst_rtsp_server_get_bound_port(restreamServer));

    g_main_loop_run(loop);
}

}
