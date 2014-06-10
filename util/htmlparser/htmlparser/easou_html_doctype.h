/*
 * easou_html_doctype.h
 *
 * 文档类型相关。确定wap1.0，wap2.0，web
 *
 *  Created on: 2012-4-17
 *      Author: sandy
 */

#ifndef EASOU_HTML_DOCTYPE_H_
#define EASOU_HTML_DOCTYPE_H_

#include "easou_html_dom.h"

/**
 * 根据树确定文档类型
 * @param:
 * html_tree, [in], 解析好的html树
 * url, [in], URL地址
 * @return:
 * 0, 成功；-1，失败
 */
int determine_doctype_from_tree(html_tree_t *html_tree, const char *url);

#endif /* EASOU_HTML_DOCTYPE_H_ */