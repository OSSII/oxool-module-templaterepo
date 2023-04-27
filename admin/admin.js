/* -*- js-indent-level: 8 -*- */
/*

*/
/* global Admin $ SERVICE_ROOT */
Admin.SocketBroker({

	_module: null,

	// 完整的 API 位址
	_fullServiceURI: "",

	onSocketOpen: function() {
		this.socket.send('getModuleInfo'); // 取得本模組資訊
		this.socket.send('getList'); // 取得 Mac IP 列表
	},

	onSocketClose: function() {
		console.debug('on socket close!');
	},

	onSocketMessage: function(e) {
		let textMsg = e.data;
		if (typeof e.data !== 'string') {
			textMsg = '';
		}

		// 模組資訊
		if (textMsg.startsWith('moduleInfo ')) {
			let jsonIdx = textMsg.indexOf('{');
			if (jsonIdx > 0) {
				this._module = JSON.parse(textMsg.substring(jsonIdx));
				// 紀錄完整的 API 位址
				this._fullServiceURI = window.location.origin + SERVICE_ROOT + this._module.serviceURI;
			}
		} else if (textMsg.startsWith('macipList ')) {
			let json = JSON.parse(textMsg.substring(textMsg.indexOf('{')));
			for (const key in json) {
				const listArray = json[key];
				const element = document.getElementById(key);

				if (element) {
					this._makeList(element, listArray);
				} else {
					console.debug('unknown list:', key);
				}
			}
		// 新增一筆 Mac 來源
		} else if (textMsg.startsWith('addMacList ')) {
			let json = JSON.parse(textMsg.substring(textMsg.indexOf('{')));
			const container = document.getElementById('macList');
			container.appendChild(this._createListItem(json));
		// 新增一筆 IP 來源
		} else if (textMsg.startsWith('addIpList ')) {
			let json = JSON.parse(textMsg.substring(textMsg.indexOf('{')));
			const container = document.getElementById('ipList');
			container.appendChild(this._createListItem(json));
		} else if (textMsg.startsWith('updateSource ')) {
			let json = JSON.parse(textMsg.substring(textMsg.indexOf('{')));
			let listItem = this._getHostItem(document.getElementById('datarecord_' + json.id));
			listItem.setData(json);
		} else if (textMsg.startsWith('deleteSource ')) {
			const array = textMsg.split(' ');
			const id = array[1];
			let listItem = document.getElementById('datarecord_' + id);
			listItem.remove();
		} else {
			console.debug("Warning! unknown message:\n", textMsg);
		}
	},

	/**
	 * 把來源資訊放到 container 所在的 html 容器內
	 * @param {string} container - elemeny id.
	 * @param {array} aList - 含有主機資料的物件陣列，物件成員:{id: 編號, value:'來源', 'desc': '描述'}
	 */
	 _makeList: function(container, aList) {
		// 如果容器沒有 list-group class 的話，加進去
		if (!container.classList.contains('list-group')) {
			container.classList.add('list-group');
		}
		container.innerHTML = ''; // 清空所有內容
		container.innerText = ''; // 清空所有內容

		aList.forEach(function(item) {
			container.appendChild(this._createListItem(item));
		}.bind(this));

		// 製作新增來源按鈕
		let button = document.createElement('button');
		button.className = 'btn btn-primary btn-sm mt-2 col-md-3';
		button.innerHTML = '<i class="bi bi-plus me-2"></i>' + (container.id === 'macList' ? _('Add Mac address') : _('Add IP address'));
		button.type = 'button';
		button.onclick = function() {
			this._addSource(container);
		}.bind(this);
		container.parentElement.appendChild(button);
	},

	/**
	 * 建立一條主機資料的 list-item dom element(含主機資料 dom 和下拉選單 dom)
	 *
	 * @param {object} item - {id: 編號, desc:'描述', value:'來源資料'}
	 * @returns dom element
	 */
	_createListItem: function(item) {
		let listItem = document.createElement('div');
		listItem.id = 'datarecord_' + item.id;
		listItem.className = 'list-group-item list-group-item-action d-flex justify-content-between align-items-center';

		listItem.appendChild(this._createHostData(item));
		listItem.appendChild(this._createHostDropdownMenu());

		return listItem;
	},

	/**
	 * 建立主機資料的 dom element
	 * @param {object} item - 同 _createListItem;
	 * @returns dom element
	 */
	_createHostData: function(item) {
		let dataContainer = document.createElement('div'); // 容器
		dataContainer.className = 'host-data';
		dataContainer.setAttribute('record-id', item.id);

		let hostData = document.createElement('div');
		dataContainer.append(hostData);

		let hostValue = document.createElement('span');
		hostValue.className = 'host-value';
		hostValue.innerText = item.value;
		hostData.appendChild(hostValue);

		let descText = document.createElement('div');
		descText.className = 'form-text mt-0 host-desc';
		descText.innerText = item.desc;
		dataContainer.appendChild(descText);

		return dataContainer;
	},

	_createHostDropdownMenu: function() {
		let dropdown = document.createElement('div');
		dropdown.className = 'dropdown dropstart';

		let button = document.createElement('button');
		button.className = 'btn btn-secondary btn-sm dropdown-toggle';
		button.type = 'button';
		button.setAttribute('data-bs-toggle', 'dropdown');
		dropdown.appendChild(button);

		let ul = document.createElement('ul');
		ul.className = 'dropdown-menu';
		ul.style.minWidth = '1rem';
		dropdown.appendChild(ul);

		let editButton = document.createElement('button');
		editButton.className = 'dropdown-item';
		editButton.type = 'button';
		editButton.innerHTML = '<i class="bi bi-pencil me-1"></i>' + _('Edit');
		editButton.onclick = function() {
			this._getHostItem(dropdown.parentNode).edit();
		}.bind(this);
		ul.appendChild(editButton);

		let deleteButton = document.createElement('button');
		deleteButton.className = 'dropdown-item';
		deleteButton.type = 'button';
		deleteButton.innerHTML = '<i class="bi bi-trash me-1"></i>' + _('Delete');
		deleteButton.onclick = function() {
			this._getHostItem(dropdown.parentNode).remove();
		}.bind(this);
		ul.appendChild(deleteButton);

		return dropdown;
	},

	/**
	 * 解析 listItemElement 轉成 json 資料
	 * @param {dom element} listItemElement - 含有主機列表項目的 dom element
	 * @returns object 操作 host-list item 的 class
	 */
	_getHostItem: function(listItemElement) {
		let that = this;
		if (!listItemElement.classList.contains('list-group-item')) {
			return null;
		}

		return {
			_self: listItemElement, // 自己
			/**
			 * 取得自己的資料庫編號 ID
			 */
			getRecordId: function() {
				return this._self.childNodes[0].getAttribute('record-id');
			},
			/**
			 * 從頁面取得 host 資料
			 */
			getData: function() {
				let data = {
					id: this.getRecordId()
				};
				let hostData = this._self.childNodes[0].querySelectorAll('.host-value, .host-desc');
				hostData.forEach(function(element) {
					if (element.classList.contains('host-value')) {
						data.value = element.innerText;
					} else if (element.classList.contains('host-desc')) {
						data.desc = element.innerText;
					}
				});
				return data;
			},
			/**
			 * 更新自己資料(頁面會更動)
			 * @param {object} data
			 */
			setData: function(data) {
				let hostData = this._self.childNodes[0].querySelectorAll('.host-value, .host-desc');
				hostData.forEach(function(element) {
					if (element.classList.contains('host-value')) {
						element.innerText = data.value;
					} else if (element.classList.contains('host-desc')) {
						element.innerText = data.desc;
					}
				});
			},
			/**
			 * 編輯資料，會呼叫編輯 Dialog
			 */
			edit: function() {
				// 開啟編輯視窗
				that._executeEditForm({
					title: _('Edit source'), // dialog title
					data: this.getData(),
					OK: function(newData) {
						let encodedData = encodeURI(JSON.stringify(newData));
						that.socket.send('updateSource' + ' ' + encodedData);
					}.bind(this)
				});
			},
			/**
			 * 刪除資料
			 */
			remove: function() {
				// 開啟編輯視窗
				that._executeEditForm({
					title: '<span class="text-danger">' + _('Delete source') + '</span>', // dialog title
					readOnly: true, // 唯讀，不能編輯
					data: this.getData(),
					OK: function(data) {
						that.socket.send('deleteSource' + ' ' + data.id);
					}.bind(this)
				});
			}
		};
	},

	_addSource: function(container) {
		if (container) {
			let emptyData = {
				value: '',
				desc: ''
			};

			// 開啟編輯視窗
			this._executeEditForm({
				title: _('Add source'), // dialog title
				data: emptyData, // 空資料
				OK: function(data) {
					let type = (container.id === 'macList' ? 'mac' : 'ip');
					let encodedData = encodeURI(JSON.stringify(data));
					this.socket.send('addSource' + ' ' + type + ' ' + encodedData);
				}.bind(this)
			});
		}
	},

	/**
	 *
	 * @param {*} data
	 * @param {function} callback - 按下確定後回呼的 function
	 */
	 _executeEditForm: function(obj) {
		let editDialog = new bootstrap.Modal(document.getElementById('editSourceDialog'));
		let editForm = document.getElementById('editForm'); // form element
		let hostValue = document.getElementById('host-value'); // 來源輸入欄位
		let hostDesc = document.getElementById('host-desc'); // 說明輸入欄位
		let OKButton = document.getElementById('editFormOK'); // 確定按鈕

		editForm.className = 'needs-validation'; // 需要輸入檢查
		document.getElementById('hostEditTitle').innerHTML = obj.title; // Dialog title

		if (obj.data.id) {
			editForm.setAttribute("record-id", obj.data.id);
		} else {
			editForm.removeAttribute("record-id")
		}

		hostValue.value = obj.data.value;
		hostDesc.value = obj.data.desc;

		if (obj.readOnly === true) {
			hostValue.readOnly = true;
			hostDesc.readOnly = true;
		} else {
			hostValue.readOnly = false;
			hostDesc.readOnly = false;
		}

		// 按下確定按鈕
		OKButton.onclick = function(e) {
			// 檢查資料
			if (obj.readOnly === true || editForm.checkValidity()) {
				editDialog.hide(); // 關閉 dialog

				let data = {
					id: editForm.getAttribute('record-id'),
					value: hostValue.value.trim().toLowerCase(), // 轉小寫
					desc: hostDesc.value.trim()
				};

				// 有指定 OK callback 把 data 傳過去
				if (typeof obj.OK === 'function') {
					obj.OK(data);
				}
			} else {
				e.preventDefault();
				e.stopPropagation();
			}
			// 顯示檢查結果
			editForm.classList.add('was-validated');
		};

		editDialog.show(); // 顯示 Dialog
	},

});
