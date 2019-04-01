///////////////////////////////// 
//此处实现一个简易的计算加法的CGI程序，仅仅用于辅助测试
///////////////////////////////// 
#include<stdio.h>
#include<unistd.h>
#include<errno.h>
#include<string.h>
#include<stdlib.h>
#include<fcntl.h>

int GetQueryString(char output[]){
  //1、先从环境中获取到方法
  char* method = getenv("REQUEST_METHOD");
  if(method == NULL){
    //写到标准错误里面了
    fprintf(stderr,"REQUEST_METHOD failed\n");//stderr为错误指针
    return -1;
  }
  //2、如果是GET方法，就直接从环境中获取
  if(strcasecmp(method,"GET") == 0){
    char* query_string = getenv("QUERY_STRING");
    if(query_string == NULL){
      fprintf(stderr,"QUERY_STRING failed\n");
      return -1;
    }
    strcpy(output, query_string);
  }else{
  //3、如果是POST方法，先通过环境变量获取到CONTENT_LENGTH,
  //再从标准输入中读取body
    char* content_length_str = getenv("CONTENT_LENGTH");
    if(content_length_str == NULL){
      fprintf(stderr, "CONTENT_LENGTH failed\n");
      return -1;
    }
    int content_length = atoi(content_length_str);
    int i=0;//表示当前已经往output中写了多少个字符了
    for(;i<content_length;++i){
      read(0,&output[i],1);//往标准输入0里读入数据
    }
    output[content_length] = '\0';//确保读取结束
  }
  return 0;
}
int main()
{
  //1、先获取参数（方法，query_string,body）
  char buf[1024*4]={0};
  int ret = GetQueryString(buf);
  if(ret<0)
  {
    fprintf(stderr,"GetQueryString failed\n");
    return 1;
  }
  //2、解析buf中的参数，具体的解析规则，就和业务相关了
  //解析的时候需要按照客户端构造的key来进行解析
  //此处key叫做 a，b
  //a=10&b=20;
  int a,b;
  //不管是GET请求或是POST请求，接收参数的格式都为键值对相与
  //格式化输入，推荐使用字符串切分的方式解析
  sscanf(buf, "a=%d&b=%d",&a,&b);
  //3、根据业务具体要求，完成计算
  int c = a + b;
  //4、把结果构成的页面返回给浏览器
  printf("<h1>ret=%d</h1>",c);
  return 0;
}
