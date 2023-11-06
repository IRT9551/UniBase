#include "rm_scan.h"
#include "rm_file_handle.h"

/**
 * @brief 初始化file_handle和rid
 * @param file_handle
 */
RmScan::RmScan(const RmFileHandle *file_handle) : file_handle_(file_handle) {
    rid_ = find_first_record();
}

Rid RmScan::find_first_record() const {
    RmFileHandle file_hdr= *file_handle_;
    for (int page_no = RM_FIRST_RECORD_PAGE; page_no < file_hdr.get_file_hdr().num_pages; page_no++) {
        RmPageHandle page_handle = file_handle_->fetch_page_handle(page_no);
        int num = page_handle.file_hdr->num_records_per_page;
        for (int slot_no = 0; slot_no < num; slot_no++) {
            if (file_handle_->is_record(Rid{page_no, slot_no})) {
                return Rid{page_no, slot_no};
            }
        }
    }
    return (Rid){-1, -1};
}

/**
 * @brief 找到文件中下一个存放了记录的位置
 */
void RmScan::next() {
    // Todo:
    // 找到文件中下一个存放了记录的非空闲位置，用rid_来指向这个位置
    while (rid_ != (Rid){-1,-1}) {
        RmFileHandle file_hdr= *file_handle_;
        int page_no = (file_hdr.get_file_hdr()).num_pages;
        if (rid_.page_no < page_no) {
            //printf("rid_page_no %d\n",rid_.page_no);
            RmPageHandle page_handle = file_handle_->fetch_page_handle(rid_.page_no);
            if (rid_.slot_no + 1 < page_handle.file_hdr->num_records_per_page) {
                rid_.slot_no++;
            } 
            else {
                rid_.page_no++;
                rid_.slot_no = 0;
            }
        }
        if(rid_.page_no == page_no) {
            rid_ = (Rid){-1,-1}; 
            break;
        }
        else if(file_handle_->is_record(rid_)) {
            break;
        }
    }
    //printf("Next FIN\n");
}

/**
 * @brief ​ 判断是否到达文件末尾
 */
bool RmScan::is_end() const {
    // Todo: 修改返回值
    return (rid_ == (Rid){-1,-1});
    //return false;
}

/**
 * @brief RmScan内部存放的rid
 */
Rid RmScan::rid() const {
    return rid_;
}