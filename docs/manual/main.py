# -*- coding: utf-8 -*-
"""组装并生成说明书。"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import json

import docbuild
import content1
import content2

OUT = (sys.argv[1] if len(sys.argv) > 1
       else r'D:\wb(cn)\BMS\docs\说明书\工商业储能EMS运行监控平台V1.15操作说明书.docx')

COVER = ('cover',
         '工商业储能 EMS\n运行监控平台',
         'V1.15 操作说明书',
         [('软件版本', 'V1.15'),
          ('数据库结构', 'schema v1.6'),
          ('文档版本', 'V1.15'),
          ('编写日期', '2026 年 10 月')])

BLOCKS = [COVER, ('pb',), ('toc',), ('pb',)] + content1.BLOCKS + content2.BLOCKS

PAGES = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'pages.json')


def load_pages(n):
    """读取上一轮由 Word 分页得到的页码表；数量不符则返回 None（走占位）。"""
    if not os.path.exists(PAGES):
        return None
    data = json.load(open(PAGES, encoding='utf-8-sig'))
    if len(data) != n:
        return None
    return [d['page'] for d in data]


if __name__ == '__main__':
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    n = len(docbuild.toc_entries(BLOCKS))
    pages = load_pages(n)
    print(docbuild.build(BLOCKS, OUT, pages))
    print('目录页码：%s' % ('已回填 %d 条' % n if pages else '占位（待回填）'))
