#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#include "easou_extractor_template.h"

#include "easou_html_extractor.h"
#include "easou_extractor_title.h"
#include "easou_string.h"
#include "easou_debug.h"

#include "easou_html_attr.h"
#include "easou_html_tree.h"
#include "easou_vhtml_tree.h"
#include "easou_ahtml_tree.h"
#include "easou_mark_parser.h"

#include "log.h"
#include "mysql.h"
#include "pagetranslate.h"

//includes for edb 
#include "RandomRead.h"
#include "ScannerClient.h"
#include "Scanner.h"
#include "readlocal.h"
#include "ClientConfig.h"
#include "PageDBValues.h"
#include "GlobalFun.h"
#include "dump.h"
#include "Queue.h"
#include "Log.h"

#ifdef _DEBUG_PROFILER_
#include <gperftools/profiler.h>
#endif

using namespace std;
using namespace EA_COMMON;
using namespace nsPageDB;

#define Q_BUF_SIZE (1<<21)  //2M
#define PAGE_SIZE (1<<20) //1M
#define URL_SIZE 2048

CClientConfig gConfig;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;

#ifdef TRACE_URL
int start = -1;
int url_index = 0;
#endif

struct classify_res_t
{
	char* pageGBBuf;
	int pageGBLen;
	int size;
	html_tree_t* tree;
	vtree_in_t* vtree_in;
	html_vtree_t* vtree;
	area_tree_t* atree;
};

bool createDVATrees(html_tree_t*& tree, vtree_in_t*& vtree_in, html_vtree_t*& vtree, area_tree_t*& atree)
{
	tree = html_tree_create(MAX_PAGE_SIZE);
	if (tree == NULL)
	{
		printf("html_tree_create fail\n");
		return false;
	}

	vtree_in = vtree_in_create();
	if (vtree_in == NULL)
	{
		printf("vtree_in_create fail\n");
		return false;
	}

	vtree = html_vtree_create_with_tree(tree);
	if (vtree == NULL)
	{
		printf("html_vtree_create_with_tree fail\n");
		return false;
	}

	atree = area_tree_create(NULL);
	if (atree == NULL)
	{
		printf("area_tree_create fail\n");
		return false;
	}
	return true;
}

void resetDVATrees(html_tree_t*& tree, vtree_in_t*& vtree_in, html_vtree_t*& vtree, area_tree_t*& atree)
{
	if (atree)
		mark_tree_reset(atree);
	if (vtree)
		html_vtree_reset(vtree);
	if (vtree_in)
		vtree_in_reset(vtree_in);
	if (tree)
		html_tree_reset_no_destroy((struct html_tree_impl_t*) tree);
}

bool parseDVATrees(html_tree_t*& tree, vtree_in_t*& vtree_in, html_vtree_t*& vtree, area_tree_t*& atree, char* url, int urlLen, char* page, int pageLen)
{
	timeinit();timestart();
	int ret = html_tree_parse(tree, page, pageLen);
	if (ret != 1)
	{
		printf("html_tree_parse fail, url:%s", url);
		timeend("html_tree_parse", "");
		return false;
	}timeend("html_tree_parse", "");

	timestart();
	if (VTREE_ERROR == html_vtree_parse_with_tree(vtree, vtree_in, url))
	{
		printf("html_vtree_parse_with_tree fail, url:%s", url);
		timeend("html_vtree_parse_with_tree", "");
		return false;
	}timeend("html_vtree_parse_with_tree", "");

	timestart();
	if (1 != area_partition(atree, vtree, url))
	{
		printf("area_partition fail, url:%s\n", url);
		timeend("area_partition", "");
		return false;
	}timeend("area_partition", "");

	timestart();
	if (!mark_area_tree(atree, url))
	{
		printf("mark_area_tree fail, url:%s\n", url);
		timeend("mark_area_tree", "");
		return false;
	}timeend("mark_area_tree", "");
	return true;
}

void destroyDVATrees(html_tree_t*& tree, vtree_in_t*& vtree_in, html_vtree_t*& vtree, area_tree_t*& atree)
{
	if (atree)
	{
		mark_tree_destory(atree);
		atree = NULL;
	}
	if (vtree)
	{
		html_vtree_del(vtree);
		vtree = NULL;
	}
	if (vtree_in)
	{
		vtree_in_destroy(vtree_in);
		vtree_in = NULL;
	}
	if (tree)
	{
		html_tree_del(tree);
		tree = NULL;
	}
}

bool parse_one_page(char* url, int urlLen, char* page, int pageLen, char* pageGBBuf, int& pageGBLen, int size, html_tree_t*& tree, vtree_in_t*& vtree_in, html_vtree_t*& vtree, area_tree_t*& atree)
{
	timeinit();

	PageTranslator pt;
	pageGBLen = pt.translate(page, pageLen, pageGBBuf, size);
	pageGBBuf[pageGBLen] = 0;

	timestart();
	resetDVATrees(tree, vtree_in, vtree, atree);
	if (!parseDVATrees(tree, vtree_in, vtree, atree, url, urlLen, pageGBBuf, pageGBLen))
	{
		printf("parse tree fail, url:%s\n", url);
		timeend("parse", "");
		return false;
	}timeend("parse", "");

	return true;
}

struct page_info_t
{
	char* pageBuf;
	int pageLen;
	int pageSize;
	char* url;
	int urlLen;
	int urlSize;
};

typedef struct _page_info_listnode_t
{
	page_info_t* page_info;
	_page_info_listnode_t* next;
} page_info_listnode_t;

struct page_info_list_t
{
	page_info_listnode_t* head;
	page_info_listnode_t* tail;
	int num;
};

struct pool_t
{
	page_info_list_t *supply_list;
	page_info_list_t *produce_list;
	page_info_list_t *used_list;
	int node_size;
};

struct thread_param_t
{
	pool_t* pool;
	int allnum;
	int limit;
	const char* bucketListPath;
	templates_t* dict;

	int tmplt_try;
	int tmplt_no_tmplt;
	int tmplt_rt_fail;
	int tmplt_ac_fail;
	int tmplt_hit;
	int tmplt_pic_num;
};

page_info_t* crate_page_info(int url_size, int page_size)
{
	page_info_t* page_info = new page_info_t;
	if (!page_info)
		return NULL;
	page_info->pageBuf = (char*) malloc(page_size);
	page_info->url = (char*) malloc(url_size);
	if (!page_info->pageBuf || !page_info->url)
		goto _FAIL;
	page_info->urlSize = url_size;
	page_info->pageSize = page_size;
	return page_info;
	_FAIL: if (page_info->pageBuf)
		free(page_info->pageBuf);
	if (page_info->url)
		free(page_info->url);
	delete page_info;
	return NULL;
}

page_info_listnode_t* create_pageinfo_listnode(int url_size, int page_size)
{
	page_info_listnode_t* node = new page_info_listnode_t;
	if (!node)
		return NULL;
	page_info_t* page_info = crate_page_info(url_size, page_size);
	if (!page_info)
		goto _FAIL;
	node->page_info = page_info;
	node->next = NULL;
	return node;
	_FAIL: delete node;
	return NULL;
}

void destroy_page_info(page_info_t* page_info)
{
	if (!page_info)
		return;
	if (page_info->pageBuf)
		free(page_info->pageBuf);
	if (page_info->url)
		free(page_info->url);
	delete page_info;
}

void destroy_pageinfo_listnode(page_info_listnode_t* node)
{
	if (!node)
		return;
	destroy_page_info(node->page_info);
	delete node;
}

void insert_tail(page_info_list_t* list, page_info_listnode_t* node)
{
	node->next = NULL;
	if (list->num == 0)
	{
		list->head = node;
		list->tail = node;
	}
	else
	{
		list->tail->next = node;
		list->tail = node;
	}
	list->num++;
	return;
}

void destroy_pageinfo_list(page_info_list_t* list)
{
	if (!list)
		return;
	page_info_listnode_t* node = list->head;
	while (node)
	{
		destroy_pageinfo_listnode(node);
		node = node->next;
	}
	delete list;
}

void destroy_pool(pool_t* pool)
{
	if (!pool)
		return;
	destroy_pageinfo_list(pool->supply_list);
	destroy_pageinfo_list(pool->produce_list);
	destroy_pageinfo_list(pool->used_list);
}

pool_t* create_pool(int node_size)
{
	pool_t* pool = new pool_t;
	if (!pool)
		return NULL;
	pool->supply_list = new page_info_list_t;
	pool->produce_list = new page_info_list_t;
	pool->used_list = new page_info_list_t;
	if (!pool->supply_list || !pool->produce_list || !pool->used_list)
		goto _FAIL;
	pool->supply_list->num = 0;
	pool->produce_list->num = 0;
	pool->used_list->num = 0;
	for (int i = 0; i < node_size; i++)
	{
		page_info_listnode_t* node = create_pageinfo_listnode(URL_SIZE, PAGE_SIZE);
		if (!node)
			goto _FAIL;
		insert_tail(pool->used_list, node);
	}
	pool->node_size = node_size;
	return pool;
	_FAIL: destroy_pool(pool);
	return NULL;
}

page_info_listnode_t* remove_head(page_info_list_t* list)
{
	if (list->num == 0)
		return NULL;
	page_info_listnode_t* node = list->head;
	list->head = node->next;
	list->num--;
	if (list->num == 0)
		list->tail = NULL;
	node->next = NULL;
	return node;
}

bool produce_to_pool(pool_t* pool, const char* url, int urlLen, const char* page, int pageLen)
{
	if (urlLen >= 2048 || pageLen >= PAGE_SIZE)
	{
		printf("url or page to long\n");
		return false;
	}
	page_info_listnode_t* node = remove_head(pool->used_list);
	if (!node)
		return false;
	memcpy(node->page_info->url, url, urlLen);
	node->page_info->url[urlLen] = 0;
	node->page_info->urlLen = urlLen;
	memcpy(node->page_info->pageBuf, page, pageLen);
	node->page_info->pageBuf[pageLen] = 0;
	node->page_info->pageLen = pageLen;
	insert_tail(pool->supply_list, node);
	return true;
}

int process_local_bucket(char *path, pool_t* pool)
{
	CReadLocal localfile;
	CTblRowReador* row;
	int ret;
	const char* key;
	UINT16 keylen;
	char url[UL_MAX_URL_LEN];
	int urlLen;
	char *page;
	int pageLen;

	if (localfile.open(path, &gConfig))
	{
		printf("open local bucket error, path:%s\n", path);
		return -1;
	}

	while (true)
	{
		ret = localfile.next(row);
		if (ret != 0 && ret != CPageDBValues::DB_OPR_SCAN_COMPLETE)
		{
			printf("read pagedb error\n");
			return -1;
		}
		if (ret == CPageDBValues::DB_OPR_SCAN_COMPLETE)
		{
			break;
		}

		row->getKey(key, keylen);
		urlLen = keylen < UL_MAX_URL_LEN ? keylen : (UL_MAX_URL_LEN - 1);
		memcpy(url, key, urlLen);
		*(url + urlLen) = 0;

		if (strncmp(url, "css", 3) == 0)
			continue;

		char* v;
		int vlen;
		/*
		 row->get(4, 46, v, vlen);
		 int idx_level = *(UINT8*) v;
		 if (idx_level != 4 && idx_level != 5)
		 //&& idx_level != 103 && idx_level != 104 && idx_level != 105)
		 continue;
		 row->get(4, 14, v, vlen);
		 int lang_type = *(UINT8*) v;
		 if (lang_type != 1 && lang_type != 2 && lang_type != 3)
		 continue;
		 */

		//dead link skip
		row->get(4, 2, v, vlen);
		int dead_link = *(UINT8*) v;
		if (dead_link == 1)
			continue;

		/*
		 row->get(4, 12, v, vlen);
		 int http_type = *(UINT8*) v;
		 if (http_type == 1 || http_type == 2)
		 continue;
		 */

		row->get(0, 1, v, vlen);
		string page;
		page = string(v, vlen);

		pthread_mutex_lock(&mutex);
		while (pool->used_list->num <= pool->node_size / 10)
			pthread_cond_wait(&cond, &mutex);
		if (!produce_to_pool(pool, url, urlLen, page.c_str(), page.length()))
			printf("produce to pool fail, %s\n", url);
		pthread_mutex_unlock(&mutex);
	}
	return 1;
}

void* read_thread(void* p)
{
	thread_param_t* param = (thread_param_t*) p;
	const char* bucketListPath = param->bucketListPath;
	FILE *fp = fopen(bucketListPath, "r");
	if (fp == NULL)
	{
		printf("%s not exist\n", bucketListPath);
		exit(-1);
	}
	char line[3072];
	while (fgets(line, 3072, fp))
	{
		if (*line == '#')
			continue;
		remove_tail_rnt(line);
		process_local_bucket(line, param->pool);
	}

	fclose(fp);
	return NULL;
}

classify_res_t* create_classify_res(const char* dir)
{
	classify_res_t* res = new classify_res_t;
	if (!res)
		goto _FAIL;
	res->size = PAGE_SIZE;
	res->pageGBBuf = (char*) malloc(PAGE_SIZE);
	if (!res->pageGBBuf)
		goto _FAIL;
	if (!createDVATrees(res->tree, res->vtree_in, res->vtree, res->atree))
	{
		printf("create trees fail\n");
		goto _FAIL;
	}
	return res;
	_FAIL: if (!res)
		return NULL;
	destroyDVATrees(res->tree, res->vtree_in, res->vtree, res->atree);
	if (res->pageGBBuf)
		free(res->pageGBBuf);
	delete res;
	return NULL;
}

bool tmplt_extract(const char* page, int page_len, char* url, int urlLen, classify_res_t* res, templates_t* dict, tmplt_result_t* result, thread_param_t* param, int res_no)
{
	int extract_ret = try_template_extract(dict, page, page_len, url, urlLen, res->tree, result);
	if (extract_ret != 0)
	{
		//��ȡʧ�ܣ�˵�������ڶ�Ӧ��ģ��
//		printf("no template for this url:%s\n", url);
		pthread_mutex_lock(&mutex);
		param->tmplt_try++;
		param->tmplt_no_tmplt++;
		pthread_mutex_unlock(&mutex);
		return false;
	}
	else if (result->extract_result[TMPLT_EXTRACT_REALTITLE].str_len == 0)
	{
		printf("template realtitle fail for this url:%s\n", url);
		pthread_mutex_lock(&mutex);
		param->tmplt_try++;
		param->tmplt_rt_fail++;
		pthread_mutex_unlock(&mutex);
		return false;
	}
	else if (result->extract_result[TMPLT_EXTRACT_ARTICLE].str_len == 0)
	{
		printf("template content fail for this url:%s\n", url);
		pthread_mutex_lock(&mutex);
		param->tmplt_try++;
		param->tmplt_ac_fail++;
		pthread_mutex_unlock(&mutex);
		return false;
	}
	else
	{
		//��ӡ��ȡ���
		printf("template success, url:%s\n", url);
		for (int i = 0; i < g_tmplt_extract_num; i++)
		{
			printf("%s %s\n", g_tmplt_extract_def[i].name, result->extract_result[i].str ? result->extract_result[i].str : "NULL");
		}
		//��ӡ����ʱ��
		printf("year:%d mon:%d day:%d hour:%d min:%d sec:%d\n", result->pubtime_tm.tm_year, result->pubtime_tm.tm_mon, result->pubtime_tm.tm_yday, result->pubtime_tm.tm_hour, result->pubtime_tm.tm_min, result->pubtime_tm.tm_sec);
		//��ӡ����ͼƬ��Ϣ
		//����ȡ�����Ǿ���·����δ��http://
		html_node_list_t* list = result->extract_result[TMPLT_EXTRACT_ARTICLE].selected_imgs;
		link_t img_links[300];
		int img_num = 300;
		html_tree_extract_link(list, url, img_links, img_num);
		for (int i = 0; i < img_num; i++)
		{
			printf("img_url:http://%s\n", img_links[i].url);
		}
		pthread_mutex_lock(&mutex);
		param->tmplt_try++;
		param->tmplt_hit++;
		param->tmplt_pic_num += img_num;
		pthread_mutex_unlock(&mutex);
		return true;
	}
}

bool process_one_page(char* url, int urlLen, char* page, int pageLen, classify_res_t* res)
{
	if (!parse_one_page(url, urlLen, page, pageLen, res->pageGBBuf, res->pageGBLen, res->size, res->tree, res->vtree_in, res->vtree, res->atree))
		return false;

//	char realtitle[2048];
//	int realtitle_len = html_atree_extract_realtitle(res->atree, realtitle, 2048);
//	printf("realtitle_len:%d realtitle:%s\n", realtitle_len, realtitle);
	return true;
}

void* consume_thread(void* p)
{
	static int res_no = 0;
	res_no++;
	int pid = pthread_self();
	printf("[thread-%d] created, res_no:%d\n", pid, res_no);

	thread_param_t* param = (thread_param_t*) p;
	FILE *fp;
#ifdef TRACE_URL
#ifdef TRACE_SAVE
	FILE *fpPage;
	char filename[1024];
	int ret;
#endif
#endif

	tmplt_result_t* result = create_tmplt_result(); //���߳�ʹ��tmplt_extract_data_t
	assert(result);
	classify_res_t* res = create_classify_res("../conf");
	if (res == NULL)
	{
		printf("[%d]create classify res fail\n", pid);
		return NULL;
	}

	int count = 0;
	while (true)
	{
		page_info_listnode_t* node = NULL;
		pthread_mutex_lock(&mutex);
		if (param->pool->supply_list->num <= param->pool->node_size / 10)
			pthread_cond_signal(&cond);
		node = remove_head(param->pool->supply_list);
		pthread_mutex_unlock(&mutex);
		if (!node)
		{
			if (count >= 3)
			{
				printf("[%d] waited 3 seconds, finish!\n", pid);
				break;
			}
			sleep(1);
			count++;
			continue;
		}

		count = 0;
		char* url = node->page_info->url;
		int urlLen = node->page_info->urlLen;
		char* page = node->page_info->pageBuf;
		int pageLen = node->page_info->pageLen;

#ifdef TRACE_URL
		url_index++;
		if (url_index < start)
		goto _CONTINUE;
		printf("index:%d url:%s\n", url_index, url);

#ifdef TRACE_SAVE
		ret = sprintf(filename, "pages/%d.html", url_index);
		filename[ret] = 0;
		fpPage = fopen(filename, "w");
		fwrite(page, 1, pageLen, fpPage);
		fclose(fpPage);

		fpPage = fopen("pages/url.lst", "a");
		fprintf(fpPage, "%d.html %s\n", url_index, url);
		fclose(fpPage);
#endif
#endif

		if (!process_one_page(url, urlLen, page, pageLen, res))
			goto _CONTINUE;

		tmplt_extract(res->pageGBBuf, res->pageGBLen, url, urlLen, res, param->dict, result, param, res_no);

		pthread_mutex_lock(&mutex);
		param->allnum++;

#ifdef DEBUG_INFO
		fp = fopen("urls.lst", "a");
		fprintf(fp, "%d %s\n", param->allnum, url);
		fclose(fp);
#endif

		if (param->allnum >= param->limit)
		{
			pthread_mutex_unlock(&mutex);
			del_tmplt_result(result);
			return NULL;
		}
		pthread_mutex_unlock(&mutex);

		_CONTINUE: pthread_mutex_lock(&mutex);
		insert_tail(param->pool->used_list, node);
		pthread_mutex_unlock(&mutex);
	}
	del_tmplt_result(result);
	return NULL;
}

void destroy_classify_res(classify_res_t* res)
{
	if (!res)
		return;
	destroyDVATrees(res->tree, res->vtree_in, res->vtree, res->atree);
	if (res->pageGBBuf)
		free(res->pageGBBuf);
	delete res;
}

int main(int argc, char** argv)
{
	const char* bucketListPath;
	int threadCount;

#ifdef TRACE_URL
	if (argc < 4)
	{
		printf("Usage:%s bucketListPath limit start\n", argv[0]);
		return -1;
	}
	bucketListPath = argv[1];
	int limit = atoi(argv[2]);
	start = atoi(argv[3]);
	threadCount = 1;

#ifdef TRACE_SAVE
	mkdir("pages", 0777);
#endif

#else
	if (argc < 4)
	{
		printf("Usage:%s bucketListPath threadNum limit\n", argv[0]);
		return -1;
	}
	bucketListPath = argv[1];
	threadCount = atoi(argv[2]);
	int limit = atoi(argv[3]);
#endif

	pool_t* pool = create_pool(300);
	assert(pool);

	templates_t* dict = create_templates("../conf"); //���߳�ʹ��templates_t
//	templates_t* dict = create_templates("extractor/conf"); //���߳�ʹ��templates_t
	assert(dict);
	int try_count = 0;
	int load_ret = 0;
	while ((load_ret = load_templates(dict)) != 0)
	{
		if (++try_count >= 5)
		{
			printf("already try load templates 5 times, program will exist\n");
			exit(-1);
		}
		printf("load templates from mysql fail, try later...\n");
		sleep(1);
	}
	printf("template load success\n");

	thread_param_t tparam;
	memset(&tparam, 0, sizeof(thread_param_t));
	tparam.bucketListPath = bucketListPath;
	tparam.limit = limit;
	tparam.pool = pool;
	tparam.dict = dict;

	if (!Init_Log("log.conf"))
	{
		printf("log.conf not exist\n");
		return 0;
	}
//	if (!init_css_server("./config.ini", "./log"))
//	{
//		printf("init css server fail\n");
//		exit(-1);
//	}
	if (gConfig.init("./config.ini", "pagedb") != 0)
	{
		printf("config init error\n");
		exit(-1);
	}

#ifdef _DEBUG_PROFILER_
	ProfilerStart("pt.prof");
#endif

	pthread_t r_id;
	int error = pthread_create(&r_id, NULL, read_thread, &tparam);
	if (error != 0)
	{
		printf("create read thread fail\n");
		exit(-1);
	}

	timeval endtv1, endtv2;
	gettimeofday(&endtv1, NULL);
#define TIMEDELTA_US(time1,time0) ((time1.tv_sec-time0.tv_sec)*1000*1000+(time1.tv_usec-time0.tv_usec))

	pthread_t* p_ids = new pthread_t[threadCount];
	for (int i = 0; i < threadCount; i++)
	{
		error = pthread_create(&p_ids[i], NULL, consume_thread, &tparam);
		if (error != 0)
		{
			printf("create consume thread fail\n");
			exit(-1);
		}
	}

	//waitting..
	//pthread_join(r_id, NULL);
	for (int i = 0; i < threadCount; i++)
		pthread_join(p_ids[i], NULL);

	gettimeofday(&endtv2, NULL);
	uint64_t utimes = TIMEDELTA_US(endtv2, endtv1);
	printf("allnum:%d alltime:%luus avg:%.2fus\n", tparam.allnum, utimes, (float) utimes / tparam.allnum);
	printf("tmplt_try:%d tmplt_no_hit:%d tmplt_rt_fail:%d tmplt_ac_fail:%d tmplt_hit:%d tmlt_pic_num:%d\n", tparam.tmplt_try, tparam.tmplt_no_tmplt, tparam.tmplt_rt_fail, tparam.tmplt_ac_fail, tparam.tmplt_hit, tparam.tmplt_pic_num);

#ifdef _DEBUG_PROFILER_
	ProfilerStop();
#endif

	printtime(tparam.allnum);print_counter(tparam.allnum);print_max();

	_FAIL: gConfig.destroy();
	if (p_ids)
		delete[] p_ids;
	del_templates(dict);
	return 0;
}