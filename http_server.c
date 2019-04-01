#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<sys/socket.h>
#include<arpa/inet.h>
#include<netinet/in.h>
#include<pthread.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<sys/sendfile.h>
#include<sys/wait.h>

typedef struct sockaddr sockaddr;
typedef struct sockaddr_in sockaddr_in;
#define SIZE (1024*10)   //10k
//结果代表的缓冲区：
typedef struct HttpRequst{
  char first_line[SIZE];
  char* method;
  char* url;
  char* url_path;
  char* query_string;
  int content_length;
}HttpRequst;
//读取第一行
int ReadLine(int sock, char buf[], ssize_t max_size)
{
  //按行从socket中读取数据
  //实际上浏览器发送的请求中换行符可能不一样。
  //换行符可能有：\n,\r,\r\n
  //1、循环从socket中读取字符，一次读取一个
  char c='\0';
  ssize_t i = 0;//描述当前读到的字符应该放到缓冲区的哪个下标上
  while(i < max_size)
  {
    ssize_t read_size = recv(sock,&c,1,0);
    if(read_size <= 0){
      //即使recv返回0也为失败，由于此时我们预期是至少能读到换行标记
      //此处可能是因为我们读到的报文是非法的。
      return -1;
    }
  //2、对读到的字符进行判定；
  //1)如果当前字符是\r
    if(c == '\r'){
  //  a)尝试从缓冲区读取下一个字符，判定下一个字符是\n，就把这种情况处理成 \n
      recv(sock,&c,1,MSG_PEEK);//MSG_PEEK表示只是读取数据，并不将数据从缓冲区拿走
      if(c == '\n'){
      //当下分隔符是一个 \r\n
      //接下来就把下一个 \n字符从缓冲区中删掉就可以了
        recv(sock,&c,1,0);
      }
      else{
    //  b)如果下一个字符是其他字符，就把\r 修改成\n ,
    //  (把\r 和 \n情况统一到一起)
        c = '\n';
      }
    }
  //2)如果当字符是其他字符，就把这个字符放到 buf 中
    buf[i++] = c;
  //3)如果当前字符是 \n,就退出循环，函数结束
    if(c =='\n'){
      break; 
    }
  }
  buf[i]='\0';
  return i;
}
//实现Split
ssize_t Split(char *first_line,const char* split_char,char* output[])
{
  char *tmp = NULL;//用tmp将最后一个参数的位置进行保存,tmp必须是栈上的；
  int output_index = 0;
  //strtok致命问题：主要是strtok函数线程不安全；
  //自己内部会每次保存下来本次切分的字符串，当然这个字符串的空间不是栈上的，函数调用一次会被释放掉
  //这里的空间是静态区的，这样的话，其他线程也可以访问到
  //所以使用strtok_r，其第三个参数为二级指针，我们在栈上开辟一个指针变量传过去，这样的话，我们就可以在每个线程栈上开辟空间来保存这个临时的字符串，就不会造成线程安全问题；
  //strtok_r保证线程安全问题
  char*p = strtok_r(first_line,split_char,&tmp);
  while(p != NULL){
    output[output_index++] = p;
    //后序循环调用的时候，第一个参数要填NULL；
    //此时函数就会根据上次切分的结果，继续往下切分
    p = strtok_r(NULL,split_char,&tmp);
  }
  return output_index;
}
//解析首行：请求方法，URL，版本号 GET / HTTP/1.1
int ParseFirstLine(char first_line[],char** method_ptr,char** url_ptr)
{
  //创建一个数组来装命令字符串
  char*tokens[100]={NULL};//tokens：命令牌
  //Split 切分完毕后，就会破坏掉原有的字符串，把其中的分隔符体会缓冲\0
  ssize_t n = Split(first_line," ",tokens);//根据空格进行切分
  if(n!=3){
    //如果不是被切分为三份，输入错误
    printf("first_line Split error! n=%ld\n",n);
    return -1;
  }
  //验证tokens[2]是否包含HTTP这样的关键字
  *method_ptr=tokens[0];
  *url_ptr=tokens[1];
  //返回结果
  return 0;
}
//解析url_path?query_string: /index.html?key=value
int ParseQueryString(char url[],char** url_path_ptr,char** query_string_ptr){
  //此处URL没有考虑域名的情况
  *url_path_ptr = url;
  char* p = url;
  for(;*p != '\0'; ++p){
    if(*p == '?'){
      //说明带有query_string
      //将？替换成 \0
      *p = '\0';
      //p指向?，从p+1的位置解析query_string
      *query_string_ptr = p+1;
      return 0;
    }
  }
  //如果循环结束都没找到？说明URL不存在query_string
  *query_string_ptr=NULL;
  return 0;
}
int HandlerHeader(int new_sock,int * content_length_ptr){
  const char* content_length_str = "Content-Length: ";
  *content_length_ptr = 0;
  while(1){  
    //定义一个缓冲区，往缓冲区里读数据
    char buf[SIZE] = {0};
    if(ReadLine(new_sock,buf,sizeof(buf))<=0){
      printf("ReadLine failed\n");
      return -1;
    }
    if(strcmp(buf,"\n")==0){
      //读到了空行，此时header部分就结束了
      return 0;
    }
    //1、找到Content_Length
    if(strncmp(buf,content_length_str,strlen(content_length_str))==0){
      *content_length_ptr=
        atoi(buf + strlen(content_length_str));
    //此处不应该直接return或者break，应该继续往后读取，
    //本函数还有另一重含义：把接收的数据都读出来，也从缓冲区删掉，避免粘包问题 
      //return  0;
    }
  }
  return 0;
}
int Handler404(int new_sock)
{
  printf("In Handler404\n");
  //int*ptr = NULL;
  //*ptr = 10;
  //构造一个错误处理的页面
  //严格遵守HTTP响应格式
  const char* first_line = "HTTP/1.1 404 Not Found\n";
  //此处的代码我们可以先不加header
  //Const-Type可以让浏览器自动识别
  //Contest-Length可以通过关闭socket的方式告知浏览器已读完
  send(new_sock,first_line,strlen(first_line),0);
  const char* blank_line = "\n";
  //body部分的内容就是 HTML
  //set encoding就能得到文字的编码格式
  const char* body = "<head><meta http-equiv=\"content-type\"\
                      content=\"text/html;charset=utf-8\"></head>\
                      <h1>404!喵星人吃掉了您的页面</h1>";
  char content_length[SIZE] = {0};
  sprintf(content_length,"content_length:%lu\n",strlen(body));

  send(new_sock, first_line,strlen(first_line),0);
  send(new_sock, content_length,strlen(content_length),0);
  send(new_sock, blank_line,strlen(blank_line),0);
  send(new_sock, body,strlen(body),0);
  return 0;
}

int IsDir(const char* file_path)
{
  struct stat st;
  int ret = stat(file_path,&st);
  if(ret < 0){
    //此处不是目录
    return 0;
  }
  if(S_ISDIR(st.st_mode)){
    return 1;
  }
  return 0;
}

ssize_t GetFileSize(const char *file_path)
{
  struct stat st;
  int ret = stat(file_path, &st);
  if(ret<0)
  {
    return 0;
  }
  return st.st_size;
}
int WriteStaticFile(int new_sock, const char* file_path)
{
  //1、打开文件，失败返回404
  int fd = open(file_path, O_RDONLY);
  if(fd < 0)
  {
    perror("open");
    return 404;
  }
  //2、构造HTTP响应报文
  const char * first_line = "HTTP/1.1 200 OK\n";
  send(new_sock, first_line, strlen(first_line), 0);
  const char * blank_line = "\n";
  send(new_sock, blank_line, strlen(blank_line), 0);
  //3、读文件内容并且写到socket中
  //char c = '\0';
  //while(read(new_sock, &c, 1) > 0){
  //  send(new_sock, &c,1,0);
  //此处换用更高效的sendfile来完成文件传输操作
  ssize_t file_size = GetFileSize(file_path);
  sendfile(new_sock,fd,NULL,file_size);//最后一个参数用stat函数获取大小
  //4、关闭文件
  close(fd);
  return 200;
}
void HanderFilePath(const char *url_path, char file_path[])
{
  //url 是以/开头的，所以不需要wwwroot之后显示
  sprintf(file_path,"./wwwroot%s",url_path);
  //如果url_path指向目录，就在目录后面拼接上index.html作为默认访问的文件
  //如何识别url_path指向的文件到底是普通文件还是目录？
  //a）如果url_path以/结尾；

  if(file_path[strlen(file_path)-1]=='/'){
    strcat(file_path,"index.html");
  }else{
  //b）url_path没以/结尾，就根据文件属性来判断
    if(IsDir(file_path)){
      strcat(file_path, "/index.html");
    }
  }
  ///printf("%s\n",file_path);
}
int HandlerStaticFile(int new_sock, const HttpRequst* req){
    //1、根据上面解析出的url_path,获取到对应的真实文件路径
  //此时HTTP服务器根目录叫 ./wwwroot
  //在URL中写path就叫做 /image/1.jpg
    char file_path[SIZE] = {0};
    HanderFilePath(req->url_path,file_path);
    //2、打开文件，把文件中的内容读取出来，并写入socket
    int err_code = WriteStaticFile(new_sock,file_path);
    return err_code;
}
int HandlerCGIFather(int new_sock,
    int father_read,int father_write,
    const HttpRequst* req){
  //  a）如果是POST请求，读出body写入管道
  // 剩下的动态生成页面的过程都交给子进程来完成
 if(strcasecmp(req->method,"POST") == 0) 
 {
   //从管道里读写数据；
   //注意1：
   //sendfile可以将一个文件中的数据之间拷到另一个文件中，
   //但是这里用不了，sendfile要求目标文件必须是socket，因此不符合
   //根据body的长度决定读取多少个字节
   int i = 0;
   char c = '\0';
   for(;i<req->content_length;++i){
     read(new_sock,&c,1);//逐个读写
     write(father_write,&c,1);
   }
   //注意2：下面一次性读入可能会导致body没有被完全读完
   //即使缓冲区足够长，但是read有可能会被打断。
   //比较靠谱的方式还是要循环进行read，然后校验read返回结果的和
   //是否达到了Content_Length
   /*
   char buf[10*1024] = {0};
   read(new_sock,buf,sizeof(buf)-1);
   write(father_write,buf,req->content_length);*/
 }
  //  b)构造HTTP响应的首行、header、空行;
  const char* first_line = "HTTP/1.1 200 OK\n";
  send(new_sock, first_line, strlen(first_line),0);
  printf("%s",first_line);
  //此处为了简单，可以先不写header
  const char* blank_line = "\n";
  send(new_sock,blank_line,strlen(blank_line),0);
  //  c）从管道中读取数据（子进程动态生成的页面）
  //  ，把这个数据也写到socket中
  //此处也不方便用sendfile，数据长度不确定
  char c = '\0';
   while(read(father_read,&c,1) > 0){
     //printf("%c", c);
     write(new_sock,&c,1);
     fflush(stdout);
   }
 
  //  d）进程等待，回收子进程资源
  //  如果要进行进程等待，那么最好使用waitpid，
  //   waitpid(,NULL,0);
  //  以保证当前线程回收的子进程就是自己当年创建的那个子进程
  //  更简洁的做法是忽略SIGCHLD信号
  return 200;
}
int HandlerCGIChild(int child_read,int child_write,
    const HttpRequst* req){
  //子进程读数据，进行程序替换：直接从管道中读数据不可取
  //a）设置环境变量保存信息：
  //  (REQUEST_METHOD, QUERY_STRING, CONTENT_LENGTH)
  //    设置环境变量是为了程序替换之后，子进程仍然能获取到这些值
  //    如果以上信息用管道告知替换之后的程序也是可以的，
  //    由于要遵守CGI标准规定，所以用环境变量传递以上信息
  //  注意：设置环境变量的步骤，不能由父进程来进行。虽然子进程能够
  //  继承父进程的环境变量，由于同一时刻会有多个请求，每个请求
  //  都在尝试修改父进程的环境变量，就会产生类似线程安全的问题，
  //  导致子进程不能正确的获取这些信息。
  char method_env[SIZE]={0};
  //REQUEST_METHOD=GET 将方法设置进来
  sprintf(method_env,"REQUEST_METHOD=%s", req->method);
  putenv(method_env);
  if(strcasecmp(req->method,"GET")==0){
    //设置QUERY_STRING
    char query_string_env[SIZE]={0};
    sprintf(query_string_env,"QUERY_STRING=%s",
        req->query_string);
    putenv(query_string_env);
  }else{
  //设置CONTENT_LENGTH
    char content_length_env[SIZE]={0};
    sprintf(content_length_env,"CONTENT_LENGTH=%d",
        req->content_length);
    putenv(content_length_env);
  }
  //  b）把标准输入和标准输出重定向到管道上，
  //    此时CGI程序读写标准输入输出就相当于读写管道。
  dup2(child_read,0);
  dup2(child_write,1);
  //  c）子进程进行性程序替换
  //  （需要先找到哪个CGI可执行程序，然后在使用exec函数进行替换）
  //  替换成功之后，动态页面都交给CGI进行计算生成
  //  假设 url_path 的值为 /cgi-bin/test 
  //  对应的CGI路径就是 ./wwwroot/cgi-bin/test
  char file_path[SIZE]={0};
  HanderFilePath(req->url_path,file_path);

  //  exec
  //  l(变长参数列表) lp  le
  //    :p表示不需要告诉一个完整的去路径，只需要告诉程序名就可以在path中自动搜索（file_path已经是完整路径，不需要）
  //    :v表示可以让用户自己添加环境变量（需要数组）
  //  v(数组)  vp  ve  
  //  直接execl
  int err = execl(file_path,file_path,NULL);
  if(err < 0)
    printf("exec error\n");
  //第一个参数可执行程序路径
  //第二个，argv[0]:就是可执行程序的路径
  //第三个参数，不需要其他命令行参数，NULL表示命令行参数结束
  //  d）替换失败的错误处理
  //  路径可能错误，需要错误处理，让进程终止：
  //  子进程就是为了替换而存在的，如果替换失败，
  //  子进程就没有存在的必要了，如果进程没终止，
  //  子进程代码会和父进程代码一致，就可能会对父进程代码造成干扰
  //  就没有存在的价值
  exit(0);

}
//生成动态页面
int HandlerCGI(int new_sock, const HttpRequst* req){
  //1、创建一对匿名管道
  int fd1[2],fd2[2];
  int err_code = 200;
  pipe(fd1);
  pipe(fd2);
  int father_read = fd1[0];
  int child_write = fd1[1];
  int child_read = fd2[0];
  int father_write = fd2[1];
  //2、创建子进程fork
  pid_t ret = fork();//返回的ret就是子进程的PID
  if(ret > 0){
  //3、父进程核心流程：
    //father
    //此处先把不必要的文件描述符关掉
    //为了保证后面父进程从管道中读数据的时候，read能够正确返回
    //不阻塞，后面的代码中会循环从管道中读数据，读到EOF就认为
    //读完了，循环退出，
    //而对于管道来说，必须所有的写端关闭再进行读，才读到EOF
    //而这里所有的写端包括父子进程的写端，子进程的写端会随着
    //子进程的终止而自动关闭。
    //父进程的写端，就可以在此处直接关闭
    //（父进程自己也不需要使用这个写端）
    close(child_read);
    close(child_write);
    HandlerCGIFather(new_sock,father_read,father_write,req);
  }else if(ret == 0){
  //4、子进程核心流程：
    //child
    HandlerCGIChild(child_read,child_write,req);
  }else{
    perror("fork");
    err_code=404;
    goto END;
  }
END:
  close(child_read);
  close(child_write);
  close(father_read);
  close(father_write);

  return err_code;
}

void HandlerRequest(int new_sock){
  //完成一次请求处理过程的函数:真正完成new_sock的数据读写过程
 //1、读取请求并解析；
 // a)从socket读出HTTP请求的首行
  //实现读一行的操作：空格换为\0,读到换行符
  HttpRequst req;
  int err_code = 200;
  //定义结构体一定要注意初始化
  memset(&req,0,sizeof(req));
  if(ReadLine(new_sock,req.first_line,sizeof(req.first_line)-1)<0){
    printf("ReadLine first_line failed\n");
    err_code=404;
    //构造404响应代码
    goto END;
  }
 // b)解析首行，获取到方法，URL，版本号（不用）；
  if(ParseFirstLine(req.first_line,&req.method,&req.url)<0){
    printf("ParseFirstLine failed,first_line = %s\n",req.first_line);
    //对于错误的处理情况，统一返回404；
    err_code=404;
    //构造404响应代码
    goto END;
  }
 // c)对URL再进行解析，解析出其中的url_path,query_string
  if(ParseQueryString(req.url,&req.url_path,&req.query_string)<0){
    printf("ParseQueryString failed! url=%s\n",req.url);
    err_code=404;
    goto END;
  }
 // d)读取并解析header部分（此处为了简单，只保留Content_length, 其他的header内容直接就丢弃了）
  if(HandlerHeader(new_sock,&req.content_length)<0){
    printf("HandlerHeader failed!\n");
    err_code=404;
    goto END;
  }
 //2、根据请求的详细情况执行静态页面逻辑还是动态页面逻辑
 //  看用户有没有参数；
 //  a)如果GET请求，并且没有query_string 说明是静态页面
 //  b)如果GET请求，并且有query_string,就可以根据query_string参数内容来动态计算生成页面了
 //  c)如果POST请求，POST请求几乎不需要query_string,不管有没有都认为是动态页面
  if(strcmp(req.method,"GET")==0&&req.query_string==NULL)   {
    //生成静态页面
    err_code = HandlerStaticFile(new_sock, &req);
  }else if(strcmp(req.method,"GET")==0&&req.query_string!=NULL){
    //生成动态页面
    err_code = HandlerCGI(new_sock, &req);
  }
  else if(strcmp(req.method,"POST")==0){
    err_code = HandlerCGI(new_sock, &req);
  }else{
    printf("method not support!method=%s\n",req.method);
    err_code=404;
    goto END;
  }
END:
  //这次请求处理结束的收尾工作
  if(err_code != 200){
    Handler404(new_sock);
  }
  //此处只考虑短连接：意思是每次客户端(浏览器)给服务器发送请求之前，都是新建一个socket进行连接。
  //对于短链接来说，如果响应写完了，就可以关闭连接
  //此处由于是服务器断开连接，也就会进入TIME_WAIT状态
  //服务器可能在短时间内处理了大量的连接，出现大量的TIME_WAIT ,需要设置setsockopt
  close(new_sock);
}
//线程入口函数
//多线程的入口：处理一次客户端的请求，并且构成响应
void* ThreadEntry(void* arg)
{
  //线程入口函数，负责一次请求的完整过程
  int new_sock = (int64_t)arg;
  HandlerRequest(new_sock);//通过此函数构成响应
  return NULL;
}
void HttpServerStart(const char* ip,short port)
{
  //1、创建TCP socket；
  int lst_sock = socket(AF_INET,SOCK_STREAM,0);
  if(lst_sock < 0)
  {
    perror("socket error");
    return;
  }
  sockaddr_in addr;
  socklen_t len = sizeof(addr);
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = inet_addr(ip);
  //2、绑定端口号；
  int ret = bind(lst_sock,(sockaddr *)&addr,len);
  if(ret < 0)
  {
    perror("bind error");
    return;
  }
  //3、监听socket；
  if(listen(lst_sock,5) < 0)
  {
    perror("listen error");
    return;
  }
  printf("HttpServerStart OK\n");
  //4、进入循环，处理客户端的连接；
  while(1)
  {
    sockaddr_in peer;
    socklen_t len = sizeof(peer);
    int64_t new_sock=accept(lst_sock,(sockaddr*)&peer,&len);//int64_t为8字节和入口函数传参相匹配
    if(new_sock < 0)
    {
      perror("accept error");
      continue;
    }
    //使用多线程的方式来完成一个多个连接的并行处理
    pthread_t tid;
    pthread_create(&tid,NULL,ThreadEntry,(void *)new_sock);//最后一个参数为入口函数的参数，即new_sock
    pthread_detach(tid);
  }
}
int main(int argc,char *argv[])
{
  if(argc!=3){
    printf("Usage ./http_server.c [ip] [port]\n");
    return 1;
  }
  signal(SIGCHLD,SIG_IGN);//信号处理
  //启动http函数
  int opt=1;
  int lst_sock;
  setsockopt(lst_sock,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
  HttpServerStart(argv[1],atoi(argv[2]));//ip,port 
  return 0;
}

