CMAKE_MINIMUM_REQUIRED(VERSION 3.0)                      
PROJECT(ReverseProxy)

find_package(libevent REQUIRED)

SET(PROXY_SERVER_FILES 
	src/log.hpp 
	src/tcp_client.h 
	src/tcp_client.cpp 
	src/proxy_server.cpp 
	)
	
SET(PROXY_FORWARD_FILES 
	src/log.hpp 
	src/tcp_server.h 
	src/tcp_server.cpp 
	src/proxy_forward.cpp 
	)
	
SET(LIBEVENT_LIBS 
	event
	event_core 
	event_extra
	)
	
IF (NOT WIN32)
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11")
ELSE()
	find_path(EVENT_INCLUDE_DIR event2/event.h)
	include_directories(${EVENT_INCLUDE_DIR})
ENDIF () 

ADD_EXECUTABLE(proxy_forward ${PROXY_FORWARD_FILES})
TARGET_LINK_LIBRARIES(proxy_forward ${LIBEVENT_LIBS})

ADD_EXECUTABLE(proxy_server ${PROXY_SERVER_FILES})
TARGET_LINK_LIBRARIES(proxy_server ${LIBEVENT_LIBS})
