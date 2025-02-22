// Copyright 2019 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

import 'chrome://resources/cr_elements/cr_action_menu/cr_action_menu.m.js';
import 'chrome://resources/cr_elements/cr_button/cr_button.m.js';
import 'chrome://resources/cr_elements/cr_dialog/cr_dialog.m.js';
import 'chrome://resources/cr_elements/cr_icon_button/cr_icon_button.m.js';
import 'chrome://resources/cr_elements/cr_icons_css.m.js';
import 'chrome://resources/cr_elements/cr_input/cr_input.m.js';
import 'chrome://resources/cr_elements/cr_toast/cr_toast.m.js';
import 'chrome://resources/cr_elements/hidden_style_css.m.js';
import './strings.m.js';

import {assert} from 'chrome://resources/js/assert.m.js';
import {isMac} from 'chrome://resources/js/cr.m.js';
import {FocusOutlineManager} from 'chrome://resources/js/cr/ui/focus_outline_manager.m.js';
import {loadTimeData} from 'chrome://resources/js/load_time_data.m.js';
import {Debouncer, html, microTask, PolymerElement} from 'chrome://resources/polymer/v3_0/polymer/polymer_bundled.min.js';

import {BrowserProxy} from './browser_proxy.js';

/**
 * @enum {number}
 * @const
 */
const ScreenWidth = {
  NARROW: 0,
  MEDIUM: 1,
  WIDE: 2,
};

/**
 * @param {!HTMLElement} tile
 * @private
 */
function resetTilePosition(tile) {
  tile.style.position = '';
  tile.style.left = '';
  tile.style.top = '';
}

/**
 * @param {!HTMLElement} tile
 * @param {!{x: number, y: number}} point
 * @private
 */
function setTilePosition(tile, {x, y}) {
  tile.style.position = 'absolute';
  tile.style.left = `${x}px`;
  tile.style.top = `${y}px`;
}

/**
 * @param {!Array<!DOMRect>} rects
 * @param {number} x
 * @param {number} y
 * @return {number}
 * @private
 */
function getHitIndex(rects, x, y) {
  return rects.findIndex(
      r => x >= r.left && x <= r.right && y >= r.top && y <= r.bottom);
}

class MostVisitedElement extends PolymerElement {
  static get is() {
    return 'ntp-most-visited';
  }

  static get template() {
    return html`{__html_template__}`;
  }

  static get properties() {
    return {
      /** @private */
      columnCount_: {
        type: Boolean,
        computed: `computeColumnCount_(tiles_, screenWidth_, maxTiles_,
            visible_, showAdd_)`,
      },

      /** @private */
      customLinksEnabled_: Boolean,

      /** @private */
      dialogTileTitle_: String,

      /** @private */
      dialogTileUrl_: String,

      /** @private */
      dialogTileUrlInvalid_: {
        type: Boolean,
        value: false,
      },

      /** @private */
      dialogTitle_: String,

      /** @private */
      isRtl_: {
        type: Boolean,
        value: false,
        reflectToAttribute: true,
      },

      /**
       * Used to hide hover style and cr-icon-button of tiles while the tiles
       * are being reordered.
       * @private
       */
      reordering_: {
        type: Boolean,
        value: false,
        reflectToAttribute: true,
      },

      /** @private */
      maxTiles_: {
        type: Number,
        computed: 'computeMaxTiles_(visible_, customLinksEnabled_)',
      },

      /** @private */
      showAdd_: {
        type: Boolean,
        computed: 'computeShowAdd_(tiles_, maxTiles_, customLinksEnabled_)',
      },

      /** @private */
      showToastButtons_: Boolean,

      /** @private {!ScreenWidth} */
      screenWidth_: Number,

      /** @private {!Array<!newTabPage.mojom.MostVisitedTile>} */
      tiles_: Array,

      /** @private */
      toastContent_: String,

      /** @private */
      visible_: Boolean,
    };
  }

  /** @private {!Array<!HTMLElement>} */
  get tileElements_() {
    return /** @type {!Array<!HTMLElement>} */ (
        Array.from(this.shadowRoot.querySelectorAll('.tile:not([hidden])')));
  }

  constructor() {
    super();
    /** @private {boolean} */
    this.adding_ = false;
    const {callbackRouter, handler} = BrowserProxy.getInstance();
    /** @private {!newTabPage.mojom.PageCallbackRouter} */
    this.callbackRouter_ = callbackRouter;
    /** @private {newTabPage.mojom.PageHandlerRemote} */
    this.pageHandler_ = handler;
    /** @private {?Debouncer} */
    this.resizeDebouncer_ = null;
    /** @private {?number} */
    this.setCustomLinksEnabledListenerId_ = null;
    /** @private {?number} */
    this.setMostVisitedInfoListenerId_ = null;
    /** @private {?number} */
    this.setMostVisitedVisibleListenerId_ = null;
    /** @private {number} */
    this.actionMenuTargetIndex_ = -1;

    /**
     * This is the position of the mouse with respect to the top-left corner
     * of the tile being dragged.
     * @private {(!{x: number, y: number}|null)}
     */
    this.dragOffset_ = null;
    /** @private {!Array<!DOMRect>} */
    this.tileRects_ = [];
  }

  /** @override */
  connectedCallback() {
    super.connectedCallback();
    /** @private {boolean} */
    this.isRtl_ = window.getComputedStyle(this)['direction'] === 'rtl';
    this.setCustomLinksEnabledListenerId_ =
        this.callbackRouter_.setCustomLinksEnabled.addListener(enabled => {
          this.customLinksEnabled_ = enabled;
        });
    this.setMostVisitedInfoListenerId_ =
        this.callbackRouter_.setMostVisitedInfo.addListener(info => {
          this.visible_ = info.visible;
          this.customLinksEnabled_ = info.customLinksEnabled;
          this.tiles_ = info.tiles.slice(0, 10);
        });
    this.setMostVisitedVisibleListenerId_ =
        this.callbackRouter_.setMostVisitedVisible.addListener(visible => {
          this.visible_ = visible;
        });
    FocusOutlineManager.forDocument(document);
  }

  /** @override */
  disconnectedCallback() {
    super.disconnectedCallback();
    this.callbackRouter_.removeListener(
        assert(this.setCustomLinksEnabledListenerId_));
    this.callbackRouter_.removeListener(
        assert(this.setMostVisitedInfoListenerId_));
    this.callbackRouter_.removeListener(
        assert(this.setMostVisitedVisibleListenerId_));
    this.mediaListenerWideWidth_.removeListener(
        assert(this.boundOnWidthChange_));
    this.mediaListenerMediumWidth_.removeListener(
        assert(this.boundOnWidthChange_));
    this.ownerDocument.removeEventListener(
        'keydown', this.boundOnDocumentKeyDown_);
  }

  /** @override */
  ready() {
    super.ready();

    /** @private {!Function} */
    this.boundOnWidthChange_ = this.updateScreenWidth_.bind(this);
    /** @private {!MediaQueryList} */
    this.mediaListenerWideWidth_ = window.matchMedia('(min-width: 672px)');
    this.mediaListenerWideWidth_.addListener(this.boundOnWidthChange_);
    /** @private {!MediaQueryList} */
    this.mediaListenerMediumWidth_ = window.matchMedia('(min-width: 560px)');
    this.mediaListenerMediumWidth_.addListener(this.boundOnWidthChange_);
    this.updateScreenWidth_();
    /** @private {!function(Event)} */
    this.boundOnDocumentKeyDown_ = e =>
        this.onDocumentKeyDown_(/** @type {!KeyboardEvent} */ (e));
    this.ownerDocument.addEventListener(
        'keydown', this.boundOnDocumentKeyDown_);
  }

  /** @private */
  clearForceHover_() {
    const forceHover = this.shadowRoot.querySelector('.force-hover');
    if (forceHover) {
      forceHover.classList.remove('force-hover');
    }
  }

  /**
   * @return {number}
   * @private
   */
  computeColumnCount_() {
    if (!this.visible_) {
      return 0;
    }

    let maxColumns = 3;
    if (this.screenWidth_ === ScreenWidth.WIDE) {
      maxColumns = 5;
    } else if (this.screenWidth_ === ScreenWidth.MEDIUM) {
      maxColumns = 4;
    }


    const tileCount = Math.min(
        this.maxTiles_,
        (this.tiles_ ? this.tiles_.length : 0) + (this.showAdd_ ? 1 : 0));
    const columnCount = tileCount <= maxColumns ?
        tileCount :
        Math.min(maxColumns, Math.ceil(tileCount / 2));
    return columnCount || 3;
  }

  /**
   * @return {number}
   * @private
   */
  computeMaxTiles_() {
    return !this.visible_ ? 0 : (this.customLinksEnabled_ ? 10 : 8);
  }

  /**
   * @return {boolean}
   * @private
   */
  computeShowAdd_() {
    return this.customLinksEnabled_ && this.tiles_ &&
        this.tiles_.length < this.maxTiles_;
  }

  /**
   * If a pointer is over a tile rect that is different from the one being
   * dragged, the dragging tile is moved to the new position. The reordering
   * is done in the DOM and the by the |reorderMostVisitedTile()| call. This is
   * done to prevent flicking between the time when the tiles are moved back to
   * their original positions (by removing position absolute) and when the
   * tiles are updated via a |setMostVisitedTiles()| call.
   *
   * |reordering_| is not set to false when the tiles are reordered. The callers
   * will need to set it to false. This is necessary to handle a mouse drag
   * issue.
   * @param {number} x
   * @param {number} y
   * @private
   */
  dragEnd_(x, y) {
    this.dragOffset_ = null;
    const dragElement = this.shadowRoot.querySelector('.tile.dragging');
    if (!dragElement) {
      this.reordering_ = false;
      return;
    }
    const dragIndex = this.$.tiles.modelForElement(dragElement).index;
    dragElement.classList.remove('dragging');
    this.tileElements_.forEach(resetTilePosition);
    resetTilePosition(/** @type {!HTMLElement} */ (this.$.addShortcut));
    const dropIndex = getHitIndex(this.tileRects_, x, y);
    if (dragIndex !== dropIndex && dropIndex > -1) {
      const [draggingTile] = this.tiles_.splice(dragIndex, 1);
      this.tiles_.splice(dropIndex, 0, draggingTile);
      this.notifySplices('tiles_', [
        {
          index: dragIndex,
          removed: [draggingTile],
          addedCount: 0,
          object: this.tiles_,
          type: 'splice',
        },
        {
          index: dropIndex,
          removed: [],
          addedCount: 1,
          object: this.tiles_,
          type: 'splice',
        },
      ]);
      this.pageHandler_.reorderMostVisitedTile(draggingTile.url, dropIndex);
    }
  }

  /**
   * The positions of the tiles are updated based on the location of the
   * pointer.
   * @param {number} x
   * @param {number} y
   * @private
   */
  dragOver_(x, y) {
    const dragElement = this.shadowRoot.querySelector('.tile.dragging');
    if (!dragElement) {
      this.reordering_ = false;
      return;
    }
    const dragIndex = this.$.tiles.modelForElement(dragElement).index;
    setTilePosition(/** @type {!HTMLElement} */ (dragElement), {
      x: x - this.dragOffset_.x,
      y: y - this.dragOffset_.y,
    });
    const dropIndex = getHitIndex(this.tileRects_, x, y);
    this.tileElements_.forEach((element, i) => {
      let positionIndex;
      if (i == dragIndex) {
        return;
      } else if (dropIndex == -1) {
        positionIndex = i;
      } else if (dragIndex < dropIndex && dragIndex <= i && i <= dropIndex) {
        positionIndex = i - 1;
      } else if (dragIndex > dropIndex && dragIndex >= i && i >= dropIndex) {
        positionIndex = i + 1;
      } else {
        positionIndex = i;
      }
      setTilePosition(
          /** @type {!HTMLElement} */ (element),
          this.tileRects_[positionIndex]);
    });
  }

  /**
   * Sets up tile reordering for both drag and touch events. This method stores
   * the following to be used in |dragOver_()| and |dragEnd_()|.
   *   |dragOffset_|: This is the mouse/touch offset with respect to the
   *       top/left corner of the tile being dragged. It is used to update the
   *       dragging tile location during the drag.
   *   |reordering_|: This is property/attribute used to hide the hover style
   *       and cr-icon-button of the tiles while they are being reordered.
   *   |tileRects_|: This is the rects of the tiles before the drag start. It is
   *       to determine which tile the pointer is over while dragging.
   * @param {!HTMLElement} dragElement
   * @param {number} x
   * @param {number} y
   * @private
   */
  dragStart_(dragElement, x, y) {
    // Need to clear the tile that has a forced hover style for when the drag
    // started without moving the mouse after the last drag/drop.
    this.clearForceHover_();

    dragElement.classList.add('dragging');
    const dragElementRect = dragElement.getBoundingClientRect();
    this.dragOffset_ = {
      x: x - dragElementRect.x,
      y: y - dragElementRect.y,
    };
    const tileElements = this.tileElements_;
    // Get all the rects first before setting the absolute positions.
    this.tileRects_ = tileElements.map(t => t.getBoundingClientRect());
    if (this.showAdd_) {
      const element = /** @type {!HTMLElement} */ (this.$.addShortcut);
      setTilePosition(element, element.getBoundingClientRect());
    }
    tileElements.forEach((tile, i) => {
      setTilePosition(tile, this.tileRects_[i]);
    });
    this.reordering_ = true;
  }

  /**
   * @param {!url.mojom.Url} url
   * @return {string}
   * @private
   */
  getFaviconUrl_(url) {
    const faviconUrl = new URL('chrome://favicon2/');
    faviconUrl.searchParams.set('size', '24');
    faviconUrl.searchParams.set('show_fallback_monogram', '');
    faviconUrl.searchParams.set('page_url', url.url);
    return faviconUrl.href;
  }

  /**
   * @return {string}
   * @private
   */
  getRestoreButtonText_() {
    return loadTimeData.getString(
        this.customLinksEnabled_ ? 'restoreDefaultLinks' :
                                   'restoreThumbnailsShort');
  }

  /**
   * @return {string}
   * @private
   */
  getTileIconButtonIcon_() {
    return this.customLinksEnabled_ ? 'icon-more-vert' : 'icon-clear';
  }

  /**
   * @param {number} index
   * @return {boolean}
   * @private
   */
  isHidden_(index) {
    return index >= this.columnCount_ * 2;
  }

  /** @private */
  onAdd_() {
    this.dialogTitle_ = loadTimeData.getString('addLinkTitle');
    this.dialogTileTitle_ = '';
    this.dialogTileUrl_ = '';
    this.dialogTileUrlInvalid_ = false;
    this.adding_ = true;
    this.$.dialog.showModal();
  }

  /**
   * @param {!KeyboardEvent} e
   * @private
   */
  onAddShortcutKeyDown_(e) {
    if (e.altKey || e.shiftKey || e.metaKey || e.ctrlKey) {
      return;
    }
    if (e.key === 'Enter' || e.key === ' ') {
      e.preventDefault();
      this.onAdd_();
    }

    if (!this.tiles_ || this.tiles_.length === 0) {
      return;
    }
    const backKey = this.isRtl_ ? 'ArrowRight' : 'ArrowLeft';
    if (e.key === backKey || e.key == 'ArrowUp') {
      this.tileFocus_(this.tiles_.length - 1);
    }
  }

  /** @private */
  onDialogCancel_() {
    this.actionMenuTargetIndex_ = -1;
    this.$.dialog.cancel();
  }

  /** @private */
  onDialogClose_() {
    if (this.adding_) {
      this.$.addShortcut.focus();
    }
    this.adding_ = false;
  }

  /**
   * @param {!KeyboardEvent} e
   * @private
   */
  onDocumentKeyDown_(e) {
    if (e.altKey || e.shiftKey) {
      return;
    }

    const modifier = isMac ? e.metaKey && !e.ctrlKey : e.ctrlKey && !e.metaKey;
    if (modifier && e.key == 'z') {
      e.preventDefault();
      this.pageHandler_.undoMostVisitedTileAction();
    }
  }

  /**
   * @param {!DragEvent} e
   * @private
   */
  onDragStart_(e) {
    // |dataTransfer| is null in tests.
    if (e.dataTransfer) {
      // Remove the ghost image that appears when dragging.
      e.dataTransfer.setDragImage(new Image(), 0, 0);
    }

    this.dragStart_(/** @type {!HTMLElement} */ (e.target), e.x, e.y);
    const dragOver = e => {
      e.preventDefault();
      e.dataTransfer.dropEffect = 'move';
      this.dragOver_(e.x, e.y);
    };
    this.ownerDocument.addEventListener('dragover', dragOver);
    this.ownerDocument.addEventListener('dragend', e => {
      this.ownerDocument.removeEventListener('dragover', dragOver);
      this.dragEnd_(e.x, e.y);
      const dropIndex = getHitIndex(this.tileRects_, e.x, e.y);
      if (dropIndex !== -1) {
        this.tileElements_[dropIndex].classList.add('force-hover');
      }
      this.addEventListener('pointermove', () => {
        this.clearForceHover_();
        // When |reordering_| is true, the normal hover style is not shown.
        // After a drop, the element that has hover is not correct. It will be
        // after the mouse moves.
        this.reordering_ = false;
      }, {once: true});
    }, {once: true});
  }

  /** @private */
  onEdit_() {
    this.$.actionMenu.close();
    this.dialogTitle_ = loadTimeData.getString('editLinkTitle');
    const {title, url} = this.tiles_[this.actionMenuTargetIndex_];
    this.dialogTileTitle_ = title;
    this.dialogTileUrl_ = url.url;
    this.dialogTileUrlInvalid_ = false;
    this.$.dialog.showModal();
  }

  /** @private */
  onRestoreDefaultsClick_() {
    this.$.toast.hide();
    this.pageHandler_.restoreMostVisitedDefaults();
  }

  /** @private */
  async onRemove_() {
    this.$.actionMenu.close();
    await this.tileRemove_(this.actionMenuTargetIndex_);
    this.actionMenuTargetIndex_ = -1;
  }

  /** @private */
  async onSave_() {
    let newUrl;
    try {
      newUrl = new URL(
          this.dialogTileUrl_.includes('://') ?
              this.dialogTileUrl_ :
              `https://${this.dialogTileUrl_}/`);
      if (!['http:', 'https:'].includes(newUrl.protocol)) {
        throw new Error();
      }
    } catch (e) {
      this.dialogTileUrlInvalid_ = true;
      return;
    }

    this.dialogTileUrlInvalid_ = false;

    this.$.dialog.close();
    let newTitle = this.dialogTileTitle_.trim();
    if (newTitle.length === 0) {
      newTitle = this.dialogTileUrl_;
    }
    if (this.adding_) {
      const {success} = await this.pageHandler_.addMostVisitedTile(
          {url: newUrl.href}, newTitle);
      this.toast_(success ? 'linkAddedMsg' : 'linkCantCreate', success);
    } else {
      const {url, title} = this.tiles_[this.actionMenuTargetIndex_];
      if (url.url !== newUrl.href || title !== newTitle) {
        const {success} = await this.pageHandler_.updateMostVisitedTile(
            url, {url: newUrl.href}, newTitle);
        this.toast_(success ? 'linkEditedMsg' : 'linkCantEdit', success);
      }
      this.actionMenuTargetIndex_ = -1;
    }
  }

  /**
   * @param {!Event} e
   * @private
   */
  onTileIconButtonClick_(e) {
    e.preventDefault();
    const {index} = this.$.tiles.modelForElement(e.target.parentElement);
    if (this.customLinksEnabled_) {
      this.actionMenuTargetIndex_ = index;
      this.$.actionMenu.showAt(e.target);
    } else {
      this.tileRemove_(index);
    }
  }

  /**
   * @param {!KeyboardEvent} e
   * @private
   */
  onTileKeyDown_(e) {
    if (e.altKey || e.shiftKey || e.metaKey || e.ctrlKey) {
      return;
    }

    if (e.key != 'ArrowLeft' && e.key != 'ArrowRight' && e.key != 'ArrowUp' &&
        e.key != 'ArrowDown' && e.key != 'Delete') {
      return;
    }

    const {index} = this.$.tiles.modelForElement(e.target);
    if (e.key == 'Delete') {
      this.tileRemove_(index);
      return;
    }

    const advanceKey = this.isRtl_ ? 'ArrowLeft' : 'ArrowRight';
    const delta = (e.key == advanceKey || e.key == 'ArrowDown') ? 1 : -1;
    this.tileFocus_(Math.max(0, index + delta));
  }

  /** @private */
  onUndoClick_() {
    this.$.toast.hide();
    this.pageHandler_.undoMostVisitedTileAction();
  }

  /**
   * @param {!TouchEvent} e
   * @private
   */
  onTouchStart_(e) {
    if (this.reordering_) {
      return;
    }
    const tileElement = /** @type {HTMLElement} */ (e.composedPath().find(
        el => el.classList && el.classList.contains('tile')));
    if (!tileElement) {
      return;
    }
    const {pageX, pageY} = e.changedTouches[0];
    this.dragStart_(tileElement, pageX, pageY);
    const touchMove = e => {
      const {pageX, pageY} = e.changedTouches[0];
      this.dragOver_(pageX, pageY);
    };
    const touchEnd = e => {
      this.ownerDocument.removeEventListener('touchmove', touchMove);
      tileElement.removeEventListener('touchend', touchEnd);
      tileElement.removeEventListener('touchcancel', touchEnd);
      const {pageX, pageY} = e.changedTouches[0];
      this.dragEnd_(pageX, pageY);
      this.reordering_ = false;
    };
    this.ownerDocument.addEventListener('touchmove', touchMove);
    tileElement.addEventListener('touchend', touchEnd, {once: true});
    tileElement.addEventListener('touchcancel', touchEnd, {once: true});
  }

  /**
   * @param {number} index
   * @private
   */
  tileFocus_(index) {
    if (index < 0) {
      return;
    }
    const tileElements = this.tileElements_;
    if (index < tileElements.length) {
      tileElements[index].focus();
    } else if (this.showAdd_ && index === tileElements.length) {
      this.$.addShortcut.focus();
    }
  }

  /**
   * @param {string} msgId
   * @param {boolean} showButtons
   * @private
   */
  toast_(msgId, showButtons) {
    this.toastContent_ = loadTimeData.getString(msgId);
    this.showToastButtons_ = showButtons;
    this.$.toast.show();
  }

  /**
   * @param {number} index
   * @return {!Promise}
   * @private
   */
  async tileRemove_(index) {
    const {title, url} = this.tiles_[index];
    const {success} = await this.pageHandler_.deleteMostVisitedTile(url);
    this.toast_(success ? 'linkRemove' : 'linkCantRemove', success);
    this.tileFocus_(index);
  }

  /** @private */
  updateScreenWidth_() {
    if (this.mediaListenerWideWidth_.matches) {
      this.screenWidth_ = ScreenWidth.WIDE;
    } else if (this.mediaListenerMediumWidth_.matches) {
      this.screenWidth_ = ScreenWidth.MEDIUM;
    } else {
      this.screenWidth_ = ScreenWidth.NARROW;
    }
  }
}

customElements.define(MostVisitedElement.is, MostVisitedElement);
