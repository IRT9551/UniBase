#include "buffer_pool_manager.h"

/**
 * @description: 从free_list或replacer中得到可淘汰帧页的 *frame_id
 * @return {bool} true: 可替换帧查找成功 , false: 可替换帧查找失败
 * @param {frame_id_t*} frame_id 帧页id指针,返回成功找到的可替换帧id
 */
bool BufferPoolManager::find_victim_page(frame_id_t* frame_id) {
    // Todo:
    // 1 使用BufferPoolManager::free_list_判断缓冲池是否已满需要淘汰页面
    // 1.1 未满获得frame
    // 1.2 已满使用lru_replacer中的方法选择淘汰页面

    if (!free_list_.empty()) {
        // 缓冲池还有空闲帧，直接从空闲帧中选择一个作为淘汰页
        *frame_id = free_list_.front();
        free_list_.pop_front();
        return true;
    }

    // 缓冲池已满，通过置换策略选择一个淘汰页
    if (replacer_->victim(frame_id)) {
        PageId page_id = pages_[*frame_id].get_page_id();
        page_table_.erase(page_id);  // 从页表中移除淘汰页
        return true;
    }

    return false;  // 找不到淘汰页
}

/**
 * @description: 更新页面数据, 如果为脏页则需写入磁盘，再更新为新页面，更新page元数据(data, is_dirty, page_id)和page table
 * @param {Page*} page 写回页指针
 * @param {PageId} new_page_id 新的page_id
 * @param {frame_id_t} new_frame_id 新的帧frame_id
 */
void BufferPoolManager::update_page(Page *page, PageId new_page_id, frame_id_t new_frame_id) {
    // Todo:
    // 1 如果是脏页，写回磁盘，并且把dirty置为false
    // 2 更新page table
    // 3 重置page的data，更新page id

    if (page->is_dirty()) {
        // 如果是脏页，将其写回磁盘
        disk_manager_->write_page(page->get_page_id().fd, page->get_page_id().page_no, page->get_data(), PAGE_SIZE);
        page->is_dirty_ = false;
    }
    page->pin_count_ = 0;

    // 更新页表
    page_table_.erase(page->get_page_id());
    page_table_[new_page_id] = new_frame_id;

    // 重置页面数据并更新页面ID
    page->reset_memory();
    page->id_ = new_page_id;
}

/**
 * @description: 从buffer pool获取需要的页。
 *              如果页表中存在page_id（说明该page在缓冲池中），并且pin_count++。
 *              如果页表不存在page_id（说明该page在磁盘中），则找缓冲池victim page，将其替换为磁盘中读取的page，pin_count置1。
 * @return {Page*} 若获得了需要的页则将其返回，否则返回nullptr
 * @param {PageId} page_id 需要获取的页的PageId
 */
Page* BufferPoolManager::fetch_page(PageId page_id) {
    //Todo:
    // 1.     从page_table_中搜寻目标页
    // 1.1    若目标页有被page_table_记录，则将其所在frame固定(pin)，并返回目标页。
    // 1.2    否则，尝试调用find_victim_page获得一个可用的frame，若失败则返回nullptr
    // 2.     若获得的可用frame存储的为dirty page，则须调用updata_page将page写回到磁盘
    // 3.     调用disk_manager_的read_page读取目标页到frame
    // 4.     固定目标页，更新pin_count_
    // 5.     返回目标页

    std::scoped_lock lock{latch_};
    // 检查页面是否已经在内存中
    auto it = page_table_.find(page_id);
    if (it != page_table_.end()) {
        frame_id_t frame_id = it->second;
        Page* page = &pages_[frame_id];
        replacer_->pin(frame_id);  // 在使用页面之前将其固定
        pages_[frame_id].pin_count_++;
        return page;
    }

    // 页面不在内存中，需要从磁盘读取并放入缓冲池
    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;  // 找不到可用的帧页
    }

    Page* page = &pages_[frame_id];
    // 2.     若获得的可用frame存储的为dirty page，则须调用updata_page将page写回到磁盘
    if (page->is_dirty()) {
        update_page(page, page_id, frame_id);
    }

    // 从磁盘读取目标页到缓冲池
    disk_manager_->read_page(page_id.fd, page_id.page_no, page->get_data(), PAGE_SIZE); //?

    page_table_[page_id] = frame_id;
    page->id_ = page_id;

    replacer_->pin(frame_id);
    page->pin_count_=1;

    return page;
}

/**
 * @description: 取消固定pin_count>0的在缓冲池中的page
 * @return {bool} 如果目标页的pin_count<=0则返回false，否则返回true
 * @param {PageId} page_id 目标page的page_id
 * @param {bool} is_dirty 若目标page应该被标记为dirty则为true，否则为false
 */
bool BufferPoolManager::unpin_page(PageId page_id, bool is_dirty) {
    // Todo:
    // 0. lock latch
    // 1. 尝试在page_table_中搜寻page_id对应的页P
    // 1.1 P在页表中不存在 return false
    // 1.2 P在页表中存在，获取其pin_count_
    // 2.1 若pin_count_已经等于0，则返回false
    // 2.2 若pin_count_大于0，则pin_count_自减一
    // 2.2.1 若自减后等于0，则调用replacer_的Unpin
    // 3 根据参数is_dirty，更改P的is_dirty_

    //unpin应该没问题，不该erase page_table里面的东西，只是把pin count减一

    // 检查页面是否在内存中

    std::scoped_lock lock{latch_};

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return false;  // 页面不在内存中
    }

    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];

    // 检查pin_count
    if (page->pin_count_ == 0) {
        return false;  // pin_count已经为0，无法取消固定
    }

    // 减少pin_count
    page->pin_count_--;
    if (page->pin_count_ == 0) {
        replacer_->unpin(frame_id);
    }

    // 根据参数更新is_dirty
    if(is_dirty){
        page->is_dirty_ = true;
    }

    return true;
}

/**
 * @description: 将目标页写回磁盘，不考虑当前页面是否正在被使用
 * @return {bool} 成功则返回true，否则返回false(只有page_table_中没有目标页时)
 * @param {PageId} page_id 目标页的page_id，不能为INVALID_PAGE_ID
 */
bool BufferPoolManager::flush_page(PageId page_id) {
    // Todo:
    // 0. lock latch
    // 1. 查找页表,尝试获取目标页P
    // 1.1 目标页P没有被page_table_记录 ，返回false
    // 2. 无论P是否为脏都将其写回磁盘。
    // 3. 更新P的is_dirty_
   
    std::scoped_lock lock{latch_};

    // 1. 在页表中查找目标页
    auto it = page_table_.find(page_id);
    if (it == page_table_.end() || page_id.page_no == INVALID_PAGE_ID) {
        // 1.1 目标页不在页表中，返回false
        return false;
    }

    // 2. 获取目标页P
    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];

    // 2. 无论P是否为脏页，都将其写回磁盘
    disk_manager_->write_page(page_id.fd, page_id.page_no, page->get_data(), PAGE_SIZE);

    // 3. 更新P的is_dirty_标志
    page->is_dirty_ = false;

    return true;
}

/**
 * @description: 创建一个新的page，即从磁盘中移动一个新建的空page到缓冲池某个位置。
 * @return {Page*} 返回新创建的page，若创建失败则返回nullptr
 * @param {PageId*} page_id 当成功创建一个新的page时存储其page_id
 */
Page* BufferPoolManager::new_page(PageId* page_id) {
    // 1.   获得一个可用的frame，若无法获得则返回nullptr
    // 2.   在fd对应的文件分配一个新的page_id
    // 3.   将frame的数据写回磁盘
    // 4.   固定frame，更新pin_count_
    // 5.   返回获得的page
   
   // 1. 获取一个可用的帧，如果不可用则返回nullptr

    std::scoped_lock lock{latch_};

    frame_id_t frame_id;
    if (!find_victim_page(&frame_id)) {
        return nullptr;
    }

    // 2. 为与fd关联的文件分配一个新的page_id
    page_id->page_no = disk_manager_->allocate_page(page_id->fd);

    // 3. 将帧的数据写回磁盘
    if (pages_[frame_id].is_dirty()) {
        disk_manager_->write_page(pages_[frame_id].get_page_id().fd, pages_[frame_id].get_page_id().page_no, pages_[frame_id].get_data(), PAGE_SIZE);
        pages_[frame_id].is_dirty_ = false;
    }

    // 4. 固定帧并更新pin_count_
    replacer_->pin(frame_id);
    pages_[frame_id].pin_count_ = 1;

    pages_[frame_id].id_ = *page_id;

    // 5. 返回获取到的页
    page_table_[*page_id] = frame_id;
    return &pages_[frame_id];
}

/**
 * @description: 从buffer_pool删除目标页
 * @return {bool} 如果目标页不存在于buffer_pool或者成功被删除则返回true，若其存在于buffer_pool但无法删除则返回false
 * @param {PageId} page_id 目标页
 */
bool BufferPoolManager::delete_page(PageId page_id) {
    // 1.   在page_table_中查找目标页，若不存在返回true
    // 2.   若目标页的pin_count不为0，则返回false
    // 3.   将目标页数据写回磁盘，从页表中删除目标页，重置其元数据，将其加入free_list_，返回true
    
    // 1. 在page_table_中查找目标页，若不存在返回true

    std::scoped_lock lock{latch_};

    auto it = page_table_.find(page_id);
    if (it == page_table_.end()) {
        return true;
    }

    // 2. 若目标页的pin_count不为0，则返回false
    frame_id_t frame_id = it->second;
    Page* page = &pages_[frame_id];
    if (page->pin_count_ != 0) {
        return false;
    }

    // 3. 将目标页数据写回磁盘，从页表中删除目标页，重置其元数据，将其加入free_list_
    if (page->is_dirty_) {
        disk_manager_->write_page(page->get_page_id().fd, page->get_page_id().page_no, page->get_data(), PAGE_SIZE);
    }

    page_table_.erase(it);
    page->reset_memory();
    page_id.page_no = INVALID_PAGE_ID; //?
    replacer_->unpin(frame_id);
    free_list_.push_back(frame_id);

    return true;
}

/**
 * @description: 将buffer_pool中的所有页写回到磁盘
 * @param {int} fd 文件句柄
 */
void BufferPoolManager::flush_all_pages(int fd) {
    for (auto& pair : page_table_) {
        flush_page(pair.first);
        // frame_id_t frame_id = pair.second;
        // Page* page = &pages_[frame_id];
        // if (page->get_page_id().fd == fd && page->is_dirty()) {
        //     disk_manager_->write_page(page->get_page_id().fd, page->get_page_id().page_no, page->get_data(), PAGE_SIZE);
        //     page->is_dirty_ = false;
        // }
        // flush_page(page->get_page_id());
    }
}