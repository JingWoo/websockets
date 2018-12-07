#include <iostream>
#include <vector>
#include <libwebsockets.h>
#include <thread>
#define LOCAL_RESOURCE_PATH "/usr/local/share/libwebsockets-test-server"

#define MAX_ECHO_PAYLOAD 1024

struct per_session_data__echo {
	size_t rx, tx;
	unsigned char buf[LWS_PRE + MAX_ECHO_PAYLOAD];
	unsigned int len;
	unsigned int index;
	int final;
	int continuation;
	int binary;
};

class WebsocketServer
{
public:
	WebsocketServer(int listen_port = 7681);
	~WebsocketServer();
	int Start(); //开始监听服务
	void Write(void *user, char* buffer); //对所有客户端发送消息
	void Write(struct lws *client, void *user,char* buffer); //对一个客户端发送消息
	void Receive(struct lws *client, void *user, char* buffer, int len); //接受消息的调用
	void ServerWorkerThread(void *threadid); //监听线程
private:
	static int callback_websocket(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len); 
	static int websocket_write(struct lws *wsi,void *user, char *in, int len); //向websocket中写入消息
	static void sighandler(int sig);
	struct lws_context_creation_info info;//上下文对象的信息
	struct lws_context *context;//上下文对象指针
	struct lws_protocols *lwsprotocols;//定义协议标准
	static std::vector AllClient;//所有当前连接的客户端
	static WebsocketServer* self;//this的静态调用，类似单例开发主要是为了在静态函数中使用类成员
	const char* PROTOOCOL = protoocol;//定义协议
	volatile int force_exit = 0;
	pthread_t /*pthread_echo, */pthread_service[32];
};

std::vector WebsocketServer::AllClient;
 
WebsocketServer* WebsocketServer::self = NULL;
 
void WebsocketServer::sighandler(int sig)
{
	force_exit = 1;
	lws_cancel_service(context);
}

void* WebsocketServer::ServerWorkerThread(void *threadid)
{
	while (lws_service_tsi(context, 1000, (int)(long)threadid) >= 0 && !force_exit);
	pthread_exit(NULL);
	// (void)value;
	// if (context != NULL && lwsprotocols != NULL)
	// {
	// 	int n = 0;
	// 	while (n >= 0 && !force_exit) {
	// 		lws_callback_on_writable_all_protocol(context, lwsprotocols);
	// 		n = lws_service(context, 10); //启动ws服务
	// 		usleep(250000); //休眠减少核心占用
	// 	}		
	// }
	// sleep(1);
	// //独立线程中进行循环启动ws服务为了可以回调
	// return;
}
 
WebsocketServer::WebsocketServer(int listen_port)
{
	AllClient.clear();
	self = this;
	static struct lws_protocols protocols[] = {
		/* first protocol must always be HTTP handler */
		{
			"lws-echo-protocol",		/* name - can be overriden with -e */
			callback_echo,
			sizeof(struct per_session_data__echo),	/* per_session_data_size */
			MAX_ECHO_PAYLOAD,
		},
		{
			NULL, NULL, 0		/* End of list */
		}
	};
	//注册协议
	lwsprotocols = &protocols[0];
	int opts = 0;//本软件的额外功能选项
	volatile int force_exit = 0;
	unsigned int ms, oldms = 0;
	int n = 0;
	memset(&info, 0, sizeof(info));
	//申请info内存
	/*********************passphrase 密钥*********************/
	// char passphrase[256];
	// strncpy(passphrase, optarg, sizeof(passphrase));
	// passphrase[sizeof(passphrase) - 1] = '\0';
	// info.ssl_private_key_password = passphrase;
	/*********************ssl certificate*********************/
	// int disallow_selfsigned = 0;
	// char ssl_cert[256] = LOCAL_RESOURCE_PATH"/libwebsockets-test-server.pem";
    // strncpy(ssl_cert, optarg, sizeof(ssl_cert));
	// ssl_cert[sizeof(ssl_cert) - 1] = '\0';
	// disallow_selfsigned = 1;
	/*********************ssl key*********************/
	// char ssl_key[256] = LOCAL_RESOURCE_PATH"/libwebsockets-test-server.key.pem";
	// strncpy(ssl_key, optarg, sizeof(ssl_key));
	// ssl_key[sizeof(ssl_key) - 1] = '\0';
	/*********************url*********************/
	char uri[256] = "/";
	//strncpy(uri, optarg, sizeof(uri));
	//uri[sizeof(uri) - 1] = '\0';
	/*********************threads*********************/
	int threads = 1;
	// if (threads > ARRAY_SIZE(pthread_service)) {
	// 	lwsl_err("Max threads %d\n",
	// 			ARRAY_SIZE(pthread_service));
	// 	return 1;
	// }
	info.count_threads = threads;
	/*********************max_http_header_pool*********************/
	info.max_http_header_pool = 4;
	/*********************daemonize*********************/
	int syslog_options = LOG_PID | LOG_PERROR;
	// daemonize = 1;
	// syslog_options &= ~LOG_PERROR
	/*********************log*********************/
	int debug_level = 7;
	/* we will only try to log things according to our debug_level */
	setlogmask(LOG_UPTO (LOG_DEBUG));
	openlog("lwsts", syslog_options, LOG_DAEMON);
	/* tell the library what debug level to emit and to send it to syslog */
	lws_set_log_level(debug_level, lwsl_emit_syslog);

	info.port = listen_port;
	const char *_interface = NULL;
	info.iface = _interface;
	info.protocols = protocols;
	info.ssl_cert_filepath = NULL;
	info.ssl_private_key_filepath = NULL;
	bool use_ssl = false;
	if (use_ssl) {
		info.ssl_cert_filepath = cert_path;
		info.ssl_private_key_filepath = key_path;
	}
	info.gid = -1;
	info.uid = -1;
	info.options = opts | LWS_SERVER_OPTION_VALIDATE_UTF8;
	static const struct lws_extension exts[] = {
		{
			"permessage-deflate",
			lws_extension_callback_pm_deflate,
			"permessage-deflate; client_no_context_takeover; client_max_window_bits"
		},
		{
			"deflate-frame",
			lws_extension_callback_pm_deflate,
			"deflate_frame"
		},
		{ NULL, NULL, NULL /* terminator */ }
	};
	info.extensions = exts;
	//设置info，填充info信息体,为库提供参数
	signal(SIGINT, sighandler);
}
 
int WebsocketServer::Start()
{
	context = lws_create_context(&info); //创建上下文对面，管理ws
	if (context == NULL) {
		lwsl_err("libwebsocket init failed\n");
		return -1;
	}
	int n；
	for (n = 0; n < lws_get_count_threads(context); n++)
		if (pthread_create(&pthread_service[n], NULL, ServerWorkerThread,
				   (void *)(long)n))
			lwsl_err("Failed to start service thread\n");

	/* wait for all the service threads to exit */
	void *retval;
	while ((--n) >= 0)
		pthread_join(pthread_service[n], &retval);

	/* wait for pthread_echo to exit */
	//pthread_join(pthread_echo, &retval);

	// thread t(ServerWorkerThread, 0);
    // t.detach();
	return 0;
}
 
WebsocketServer::~WebsocketServer()
{
	lws_context_destroy(context);
}
 
int WebsocketServer::websocket_write(struct lws *wsi_in, void *user, char *str, int str_size_in)
{
	struct per_session_data__echo *pss =
			(struct per_session_data__echo *)user;
	    lws_write_protocol n = LWS_WRITE_CONTINUATION;
		if (!pss->continuation) {
			if (pss->binary)
				n = LWS_WRITE_BINARY;
			else
				n = LWS_WRITE_TEXT;
			pss->continuation = 1;
		}
		if (!pss->final)
			n |= LWS_WRITE_NO_FIN;
		lwsl_info("+++ test-echo: writing %d, with final %d\n",
			  pss->len, pss->final);
		//pss->len = sprintf((char *)&pss->buf[LWS_PRE],
		//		   "hello from libwebsockets-test-echo client pid %d index %d\n",
		//		   getpid(), pss->index++);
		pss->tx += pss->len;
		n = lws_write(wsi, &pss->buf[LWS_PRE], pss->len, (lws_write_protocol)n);
		if (n < 0) {
			lwsl_err("ERROR %d writing to socket, hanging up\n", n);
			return 1;
		}
		if (n < (int)pss->len) {
			lwsl_err("Partial write\n");
			return -1;
		}
		pss->len = -1;
		if (pss->final)
			pss->continuation = 0;
		lws_rx_flow_control(wsi, 1);
}

int WebsocketServer::callback_websocket(struct lws *wsi, enum lws_callback_reasons reason, void *user, void *in, size_t len) 
{ 
	struct per_session_data__echo *pss =
			(struct per_session_data__echo *)user;

	switch (reason) {
		case LWS_CALLBACK_ESTABLISHED:
		{ 
			AllClient.push_back(wsi); 
		} 
		break;
		case LWS_CALLBACK_RECEIVE:
		{ 
			WebsocketServer::self->Receive(wsi, (void *)pss, (char *)in, len);
		}
		break;
		case LWS_CALLBACK_SERVER_WRITEABLE:
		{
			WebsocketServer::self->Write(wsi, (void *)pss, NULL);
		}
		break;
		case LWS_CALLBACK_CLOSED:
		{
			for (int i = 0; i < AllClient.size(); i++)
			{
				if (AllClient[i] == wsi)
				{
					AllClient.erase(AllClient.begin() + i);
				}
			}
		}
		break;
		case LWS_CALLBACK_CLIENT_CONFIRM_EXTENSION_SUPPORTED:
		{
			/* reject everything else except permessage-deflate */
			if (strcmp((const char *)in, "permessage-deflate"))
				return 1;
		}
		break;

		default:
			break;
	}

	return 0;
}
 
void Websocket_Server::Receive(struct lws *wsi, void *user, char* in, int len)
{
	struct per_session_data__echo *pss =
			(struct per_session_data__echo *)user;
		pss->final = lws_is_final_fragment(wsi);
		pss->binary = lws_frame_is_binary(wsi);
		lwsl_info("+++ test-echo: RX len %d final %d, pss->len=%d\n",
			  len, pss->final, (int)pss->len);

		memcpy(&pss->buf[LWS_PRE], in, len);
		pss->len = (unsigned int)len;
		pss->rx += len;

		lws_rx_flow_control(wsi, 0);
		lws_callback_on_writable(wsi);
}
 
void WebsocketServer::Write(struct lws *wsi, void *user, char* buffer)
{
	if (!wsi || !user /*|| !buffer*/)
	{
		lwsl_err("ERROR: invalid input!")
		return;
	}
	websocket_write(wsi, user, buffer, strlen(buffer));
}
 
void WebsocketServer::Write(void *user, char* buffer)
{
	for (int i = 0; i < AllClient.size(); i++)
	{
		if (AllClient[i] != NULL && buffer != NULL)
		{
			websocket_write(AllClient[i], user, buffer, strlen(buffer));
		}
	}
	return;
}