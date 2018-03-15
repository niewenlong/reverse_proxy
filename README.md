reverse_proxy
===========

反向代理程序，使用服务器中转实现外网访问内网。例如在家里访问学校内网。

# 环境需求
+ 一台有外网地址的服务器(用于转发流量)
+ 内网A(被代理网络，其他网络有访问此网络的需求，例如学校内网等)
+ 内网或外网B，此网络环境无需求，可以配置socks代理即可。

# 依赖获取
使用[Vcpkg](https://github.com/Microsoft/vcpkg)解决项目依赖，配置完成Vcpkg环境后执行如下代码
```
vcpkg install libevent
```

# 编译环境
* Visual Studio 2015

# 编译教程
```
mkdir build
cd build
cmake .. "-DCMAKE_TOOLCHAIN_FILE=D:\vcpkg\scripts\buildsystems\vcpkg.cmake"
```
上面的D:\vcpkg 换为你机器上vcpkg的安装目录，使用VS打开解决方案文件进行编译
