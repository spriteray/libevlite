
#ifndef SIDLIST_H
#define SIDLIST_H

#include <stdint.h>
#include "network.h"

//
// sidlist
//
struct sidlist {
    uint32_t count;
    uint32_t size;
    sid_t * entries;
};

// 创建
struct sidlist * sidlist_create( uint32_t size );

// 元素个数
#define sidlist_count( self ) ( ( self )->count )

// 根据下标获取元素
sid_t sidlist_get( struct sidlist * self, int32_t index );

// 添加
int32_t sidlist_add( struct sidlist * self, sid_t id );

// 批量添加
int32_t sidlist_adds( struct sidlist * self, sid_t * ids, uint32_t count );

// 追加
#define sidlist_append( self, list ) sidlist_adds( ( self ), ( list )->entries, ( list )->count )

// 移除指定下标的元素(Swap Remove)
sid_t sidlist_del( struct sidlist * self, int32_t index );

// 销毁
void sidlist_destroy( struct sidlist * self );

#endif
