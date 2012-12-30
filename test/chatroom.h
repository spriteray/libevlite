
#ifndef CHATROOM_H
#define CHATROOM_H

#include <stdint.h>

#define CHATROOM_MESSAGE_SIZE		100

#pragma pack(1)

struct CSHead
{
	uint16_t msgid;
	uint16_t length;
};

#pragma pack()

#endif
