/**
 * @file easou_css_parser.h
 * @author xunwu
 * @date 2011/06/20
 * @version 1.0(create)
 * @brief CSS节点内存分配管理.
 *
 **/

#ifndef EASOU_CSS_NODEPOOL_H_
#define EASOU_CSS_NODEPOOL_H_

#include "util/htmlparser/utils/log.h"
#include "css_dtd.h"

using namespace EA_COMMON;
/**
 * @brief	init nodepool
 * @retval   success: return 1; fail: return -1.
 * @author xunwu
 * @date 2011/06/20
 **/
int css_nodepool_init(easou_css_nodepool_t *pool, size_t size);

/**
 * @brief reset the nodepool to have only one memery node and reset the pool as never used
 * @author xunwu
 * @date 2011/06/20
 **/
void css_nodepool_reset(easou_css_nodepool_t *pool);

/**
 * @brief destroy the nodepool
 * @author xunwu
 * @date 2011/06/20
 **/
void css_nodepool_destroy(easou_css_nodepool_t *pool);

/**
 * @brief	从nodepool获取size大小的内存.
 * @retval   成功返回内存地址,失败返回NULL.
 * @see
 * @author xunwu
 * @date 2011/06/20
 **/
void *css_get_from_nodepool(easou_css_nodepool_t *pool, size_t size);

#endif /*EASOU_CSS_NODEPOOL_H_*/
