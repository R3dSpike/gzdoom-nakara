//=============================================================================
//
// Reusable in-place drop-down support for option menus.
//
// Any MENUDEF option menu that contains NakaraDropDown or a derived item must
// use Class "NakaraDropDownMenu", or a menu class derived from it.
//
//=============================================================================

class OptionMenuItemNakaraDropDownBase : OptionMenuItemOptionBase
{
	bool mDropDownOpen;
	bool mDropDownClosing;
	bool mMousePressedOutside;
	bool mOpenDown;
	double mDropDownAnimFrom;
	double mDropDownAnimTarget;
	double mDropDownAnimStartMS;
	int mPopupSelection;
	int mFirstVisible;
	int mVisibleRows;
	int mPopupX;
	int mPopupY;
	int mPopupWidth;
	int mPopupHeight;
	int mPopupRowHeight;
	int mRowY;
	int mValueX;

	protected void InitDropDown(String label, Name command, Name values,
		CVar graycheck = null, int center = 0, int graycheckVal = 0)
	{
		Super.Init(label, command, values, graycheck, center, graycheckVal);
		mPopupSelection = -1;
		mDropDownAnimFrom = 0.0;
		mDropDownAnimTarget = 0.0;
		mDropDownAnimStartMS = MSTimeF();
	}

	double GetDropDownAnimAt(double nowMS)
	{
		if (mDropDownAnimFrom ~== mDropDownAnimTarget)
		{
			return mDropDownAnimTarget;
		}

		// Menu drawing runs every rendered frame. Use real elapsed milliseconds
		// here instead of 35 Hz menu tics so the reveal remains smooth.
		double fullDurationMS = mDropDownAnimTarget > mDropDownAnimFrom ? 140.0 : 90.0;
		double durationMS = MAX(1.0, fullDurationMS * abs(mDropDownAnimTarget - mDropDownAnimFrom));
		double t = clamp((nowMS - mDropDownAnimStartMS) / durationMS, 0.0, 1.0);
		double eased;

		if (mDropDownAnimTarget > mDropDownAnimFrom)
		{
			// Cubic ease-out for opening.
			eased = 1.0 - (1.0 - t) * (1.0 - t) * (1.0 - t);
		}
		else
		{
			// Smoothstep for the quicker close.
			eased = t * t * (3.0 - 2.0 * t);
		}

		return mDropDownAnimFrom +
			(mDropDownAnimTarget - mDropDownAnimFrom) * eased;
	}

	double GetDropDownAnim()
	{
		return clamp(GetDropDownAnimAt(MSTimeF()), 0.0, 1.0);
	}

	void StartDropDownAnimation(double target)
	{
		double nowMS = MSTimeF();
		mDropDownAnimFrom = GetDropDownAnimAt(nowMS);
		mDropDownAnimTarget = clamp(target, 0.0, 1.0);
		mDropDownAnimStartMS = nowMS;
	}

	void ResetDropDownAnimation(double value)
	{
		mDropDownAnimFrom = clamp(value, 0.0, 1.0);
		mDropDownAnimTarget = mDropDownAnimFrom;
		mDropDownAnimStartMS = MSTimeF();
	}

	override void Ticker()
	{
		Super.Ticker();
		if (!mDropDownOpen) return;

		// State cleanup can stay tick-based; the visible animation itself is
		// evaluated per rendered frame in GetDropDownAnim().
		if (mDropDownClosing && GetDropDownAnim() <= 0.0)
		{
			mDropDownOpen = false;
			mDropDownClosing = false;
			mMousePressedOutside = false;
			ResetDropDownAnimation(0.0);
		}
	}

	virtual String GetDropDownText(int selection)
	{
		if (selection < 0 || selection >= OptionValues.GetCount(mValues))
		{
			return StringTable.Localize("$TXT_UNKNOWN");
		}
		return StringTable.Localize(OptionValues.GetText(mValues, selection));
	}

	// Default: use the menu's current language font for every row.
	// Derived items may override this to select a different font per entry.
	virtual Font GetDropDownItemFont(int selection)
	{
		Font font = GetItemTextFont();
		if (!font) font = SmallFont;
		return font;
	}

	protected Font GetLanguageEntryFont(int selection)
	{
		Font font;
		String languageCode;

		if (selection >= 0 && selection < OptionValues.GetCount(mValues))
		{
			languageCode = OptionValues.GetTextValue(mValues, selection);
		}

		if (languageCode ~== "jp" || languageCode ~== "ja" || languageCode ~== "jpn")
		{
			font = Font.GetFont("JPUIFONT");
		}
		else if (languageCode ~== "zh" || languageCode ~== "zh-cn" ||
			languageCode ~== "zho" || languageCode ~== "chi")
		{
			font = Font.GetFont("ZHUIFONT");
		}
		else
		{
			font = SmallFont;
		}

		if (!font) font = GetItemTextFont();
		if (!font) font = SmallFont;
		return font;
	}

	int GetDropDownRowHeight()
	{
		int rowHeight = OptionMenuSettings.mLinespacing * CleanYfac_1;
		int count = OptionValues.GetCount(mValues);

		for (int i = 0; i < count; i++)
		{
			Font font = GetDropDownItemFont(i);
			if (font)
			{
				rowHeight = MAX(rowHeight, (font.GetHeight() + 2) * CleanYfac_1);
			}
		}
		return MAX(1, rowHeight);
	}

	void UpdatePopupGeometry()
	{
		int count = OptionValues.GetCount(mValues);
		mPopupRowHeight = GetDropDownRowHeight();
		int rowHeight = mPopupRowHeight;
		int widest = 0;

		for (int i = 0; i < count; i++)
		{
			Font font = GetDropDownItemFont(i);
			if (font)
			{
				widest = MAX(widest, font.StringWidth(GetDropDownText(i)) * CleanXfac_1);
			}
		}

		mPopupWidth = MAX(104 * CleanXfac_1, widest + 30 * CleanXfac_1);
		mPopupX = mValueX - 4 * CleanXfac_1;
		mPopupX = clamp(mPopupX, 2 * CleanXfac_1,
			MAX(2 * CleanXfac_1, screen.GetWidth() - mPopupWidth - 2 * CleanXfac_1));

		int wantedRows = MIN(count, 8);
		int rowsBelow = MAX(0,
			(screen.GetHeight() - (mRowY + rowHeight) - 4 * CleanYfac_1) / rowHeight);
		int rowsAbove = MAX(0, (mRowY - 4 * CleanYfac_1) / rowHeight);
		mOpenDown = rowsBelow >= MIN(wantedRows, 4) || rowsBelow >= rowsAbove;

		if (mOpenDown)
		{
			mVisibleRows = MIN(wantedRows, MAX(1, rowsBelow));
			mPopupY = mRowY + rowHeight;
		}
		else
		{
			mVisibleRows = MIN(wantedRows, MAX(1, rowsAbove));
			mPopupY = mRowY - mVisibleRows * rowHeight - 4 * CleanYfac_1;
		}
		mPopupHeight = mVisibleRows * rowHeight + 4 * CleanYfac_1;
	}

	void EnsureSelectionVisible()
	{
		int count = OptionValues.GetCount(mValues);
		if (count <= 0) return;
		if (mVisibleRows <= 0) mVisibleRows = MIN(count, 8);

		if (mPopupSelection < 0)
		{
			mFirstVisible = 0;
			return;
		}

		mPopupSelection = clamp(mPopupSelection, 0, count - 1);
		if (mPopupSelection < mFirstVisible)
		{
			mFirstVisible = mPopupSelection;
		}
		else if (mPopupSelection >= mFirstVisible + mVisibleRows)
		{
			mFirstVisible = mPopupSelection - mVisibleRows + 1;
		}
		mFirstVisible = clamp(mFirstVisible, 0, MAX(0, count - mVisibleRows));
	}

	double GetDropDownEase()
	{
		return GetDropDownAnim();
	}

	int GetAnimatedPopupHeight()
	{
		if (!mDropDownOpen) return 0;
		return clamp(int(mPopupHeight * GetDropDownEase() + 0.5), 0, mPopupHeight);
	}

	int GetAnimatedPopupY()
	{
		int height = GetAnimatedPopupHeight();
		return mOpenDown ? mPopupY : mPopupY + mPopupHeight - height;
	}

	void OpenDropDown()
	{
		if (OptionValues.GetCount(mValues) <= 0 || isGrayed()) return;
		UpdatePopupGeometry();

		// Starting or reversing the animation keeps the currently visible height.
		if (!mDropDownOpen) ResetDropDownAnimation(0.0);
		mDropDownOpen = true;
		mDropDownClosing = false;
		StartDropDownAnimation(1.0);
		mMousePressedOutside = false;
		mPopupSelection = GetSelection();
		EnsureSelectionVisible();
		Menu.MenuSound("menu/advance");
	}

	void CloseDropDown(bool animate = true)
	{
		mMousePressedOutside = false;
		if (!mDropDownOpen)
		{
			mDropDownClosing = false;
			ResetDropDownAnimation(0.0);
			return;
		}

		double currentAnim = GetDropDownAnim();
		if (animate && currentAnim > 0.0)
		{
			mDropDownClosing = true;
			StartDropDownAnimation(0.0);
		}
		else
		{
			mDropDownOpen = false;
			mDropDownClosing = false;
			ResetDropDownAnimation(0.0);
		}
	}

	bool IsDropDownOpen()
	{
		return mDropDownOpen;
	}

	override bool Activate()
	{
		if (mDropDownOpen && !mDropDownClosing) CloseDropDown();
		else OpenDropDown();
		return true;
	}

	void MovePopupSelection(int amount)
	{
		int count = OptionValues.GetCount(mValues);
		if (count <= 0) return;
		if (mPopupSelection < 0)
		{
			mPopupSelection = amount < 0 ? count - 1 : 0;
		}
		else
		{
			mPopupSelection = (mPopupSelection + amount) % count;
			if (mPopupSelection < 0) mPopupSelection += count;
		}
		EnsureSelectionVisible();
		Menu.MenuSound("menu/cursor");
	}

	bool HandleDropDownKey(int mkey, bool fromcontroller)
	{
		if (!mDropDownOpen) return false;
		if (mDropDownClosing) return true;

		int selected;
		switch (mkey)
		{
		case Menu.MKEY_Up:
		case Menu.MKEY_Left:
			MovePopupSelection(-1);
			return true;

		case Menu.MKEY_Down:
		case Menu.MKEY_Right:
			MovePopupSelection(1);
			return true;

		case Menu.MKEY_PageUp:
			MovePopupSelection(-MAX(1, mVisibleRows));
			return true;

		case Menu.MKEY_PageDown:
			MovePopupSelection(MAX(1, mVisibleRows));
			return true;

		case Menu.MKEY_Enter:
			selected = mPopupSelection;
			CloseDropDown();
			if (selected >= 0) SetSelection(selected);
			Menu.MenuSound("menu/choose");
			return true;

		case Menu.MKEY_Back:
		case Menu.MKEY_Clear:
			CloseDropDown();
			Menu.MenuSound("menu/backup");
			return true;
		}
		return true;
	}

	bool HandleDropDownWheel(int amount)
	{
		if (!mDropDownOpen) return false;
		if (mDropDownClosing) return true;
		MovePopupSelection(amount);
		return true;
	}

	int PopupIndexAt(int x, int y)
	{
		int animatedHeight = GetAnimatedPopupHeight();
		int animatedY = GetAnimatedPopupY();
		if (animatedHeight <= 0 || x < mPopupX || x >= mPopupX + mPopupWidth ||
			y < animatedY || y >= animatedY + animatedHeight)
		{
			return -1;
		}

		int rowHeight = MAX(1, mPopupRowHeight);
		int row = (y - mPopupY - 2 * CleanYfac_1) / rowHeight;
		if (row < 0 || row >= mVisibleRows) return -1;

		int index = mFirstVisible + row;
		if (index >= OptionValues.GetCount(mValues)) return -1;
		return index;
	}

	bool HandleDropDownMouse(int type, int x, int y)
	{
		if (!mDropDownOpen) return false;
		if (mDropDownClosing) return true;

		int index = PopupIndexAt(x, y);
		if (type == Menu.MOUSE_Click)
		{
			mMousePressedOutside = index < 0;
			if (index >= 0) mPopupSelection = index;
			return true;
		}
		if (type == Menu.MOUSE_Move)
		{
			if (index >= 0) mPopupSelection = index;
			return true;
		}
		if (type == Menu.MOUSE_Release)
		{
			if (!mMousePressedOutside && index >= 0)
			{
				mPopupSelection = index;
				int selected = mPopupSelection;
				CloseDropDown();
				SetSelection(selected);
				Menu.MenuSound("menu/choose");
			}
			else
			{
				CloseDropDown();
				Menu.MenuSound("menu/backup");
			}
			return true;
		}
		return true;
	}

	override int Draw(OptionMenuDescriptor desc, int y, int indent, bool selected)
	{
		if (mCenter)
		{
			indent = screen.GetWidth() / 2;
		}

		drawLabel(indent, y,
			selected ? OptionMenuSettings.mFontColorSelection : OptionMenuSettings.mFontColor,
			isGrayed());

		int selection = GetSelection();
		String text = GetDropDownText(selection);
		drawValue(indent, y, OptionMenuSettings.mFontColorValue, text, isGrayed(), false);

		Font font = GetItemTextFont();
		mValueX = indent + CursorSpace();
		mRowY = y;
		UpdatePopupGeometry();

		int arrowX = mValueX + font.StringWidth(text) * CleanXfac_1 + 5 * CleanXfac_1;
		screen.DrawText(font, OptionMenuSettings.mFontColorValue, arrowX, y,
			(mDropDownOpen && !mDropDownClosing) ? "▲" : "▼", DTA_CleanNoMove_1, true);
		return indent;
	}

	void DrawDropDown(OptionMenuDescriptor desc)
	{
		if (!mDropDownOpen) return;
		UpdatePopupGeometry();
		EnsureSelectionVisible();

		int animatedHeight = GetAnimatedPopupHeight();
		if (animatedHeight <= 0) return;
		int animatedY = GetAnimatedPopupY();

		int borderX = MAX(1, CleanXfac_1);
		int borderY = MAX(1, CleanYfac_1);
		int rowHeight = MAX(1, mPopupRowHeight);
		Font arrowFont = GetItemTextFont();
		if (!arrowFont) arrowFont = SmallFont;

		// Draw the background and side borders at the currently visible size.
		// screen.Dim rectangles are not reliably constrained by SetClipRect on
		// every renderer, so drawing the full bottom border inside a clip can make
		// it appear on the very first animation frame. The border opposite the
		// opening anchor is therefore drawn only after the popup is fully open.
		bool fullyOpen = animatedHeight >= mPopupHeight;
		screen.Dim(Color(12, 12, 12), 0.96,
			mPopupX, animatedY, mPopupWidth, animatedHeight);
		screen.Dim(Color(180, 180, 180), 0.80,
			mPopupX, animatedY, borderX, animatedHeight);
		screen.Dim(Color(180, 180, 180), 0.80,
			mPopupX + mPopupWidth - borderX, animatedY, borderX, animatedHeight);

		if (mOpenDown)
		{
			// The top edge is attached to the option row and is visible immediately.
			screen.Dim(Color(180, 180, 180), 0.80,
				mPopupX, mPopupY, mPopupWidth, borderY);
			if (fullyOpen)
			{
				screen.Dim(Color(180, 180, 180), 0.80,
					mPopupX, mPopupY + mPopupHeight - borderY,
					mPopupWidth, borderY);
			}
		}
		else
		{
			// Upward-opening popups are anchored by their bottom edge.
			screen.Dim(Color(180, 180, 180), 0.80,
				mPopupX, mPopupY + mPopupHeight - borderY,
				mPopupWidth, borderY);
			if (fullyOpen)
			{
				screen.Dim(Color(180, 180, 180), 0.80,
					mPopupX, mPopupY, mPopupWidth, borderY);
			}
		}

		// Rows keep their full, stable geometry and are revealed by clipping.
		screen.SetClipRect(mPopupX, animatedY, mPopupWidth, animatedHeight);

		int current = GetSelection();
		for (int row = 0; row < mVisibleRows; row++)
		{
			int index = mFirstVisible + row;
			if (index >= OptionValues.GetCount(mValues)) break;

			int drawY = mPopupY + 2 * CleanYfac_1 + row * rowHeight;
			bool highlighted = index == mPopupSelection;
			if (highlighted)
			{
				screen.Dim(Color(88, 88, 88), 0.88,
					mPopupX + borderX, drawY, mPopupWidth - borderX * 2, rowHeight);
			}

			int color = highlighted ? OptionMenuSettings.mFontColorSelection :
				(index == current ? OptionMenuSettings.mFontColorMore : OptionMenuSettings.mFontColorValue);
			Font itemFont = GetDropDownItemFont(index);
			if (!itemFont) itemFont = arrowFont;
			int textHeight = itemFont ? itemFont.GetHeight() * CleanYfac_1 : rowHeight;
			int textY = drawY + MAX(0, (rowHeight - textHeight) / 2);
			screen.DrawText(itemFont, color,
				mPopupX + 5 * CleanXfac_1, textY, GetDropDownText(index),
				DTA_CleanNoMove_1, true);
		}

		if (mFirstVisible > 0)
		{
			screen.DrawText(arrowFont, OptionMenuSettings.mFontColorSelection,
				mPopupX + mPopupWidth - 13 * CleanXfac_1, mPopupY + 2 * CleanYfac_1,
				"▲", DTA_CleanNoMove_1, true);
		}
		if (mFirstVisible + mVisibleRows < OptionValues.GetCount(mValues))
		{
			screen.DrawText(arrowFont, OptionMenuSettings.mFontColorSelection,
				mPopupX + mPopupWidth - 13 * CleanXfac_1,
				mPopupY + mPopupHeight - rowHeight - 2 * CleanYfac_1,
				"▼", DTA_CleanNoMove_1, true);
		}

		screen.ClearClipRect();
	}
}

// Standard reusable CVar drop-down. MENUDEF keyword: NakaraDropDown.
class OptionMenuItemNakaraDropDown : OptionMenuItemNakaraDropDownBase
{
	CVar mCVar;

	OptionMenuItemNakaraDropDown Init(String label, Name command, Name values,
		CVar graycheck = null, int center = 0, int graycheckVal = 0)
	{
		InitDropDown(label, command, values, graycheck, center, graycheckVal);
		mCVar = CVar.FindCVar(mAction);
		return self;
	}

	override Font GetDropDownItemFont(int selection)
	{
		// Backward compatible: an existing NakaraDropDown bound to the
		// language CVar automatically uses the matching font for each row.
		if (mAction == 'language')
		{
			return GetLanguageEntryFont(selection);
		}
		return Super.GetDropDownItemFont(selection);
	}

	override int GetSelection()
	{
		int selection = -1;
		int count = OptionValues.GetCount(mValues);
		if (count > 0 && mCVar != null)
		{
			if (OptionValues.GetTextValue(mValues, 0).Length() == 0)
			{
				let value = mCVar.GetFloat();
				for (int i = 0; i < count; i++)
				{
					if (value ~== OptionValues.GetValue(mValues, i))
					{
						selection = i;
						break;
					}
				}
			}
			else
			{
				String value = mCVar.GetString();
				for (int i = 0; i < count; i++)
				{
					if (value ~== OptionValues.GetTextValue(mValues, i))
					{
						selection = i;
						break;
					}
				}
			}
		}
		return selection;
	}

	override void SetSelection(int selection)
	{
		int count = OptionValues.GetCount(mValues);
		if (count <= 0 || mCVar == null || selection < 0 || selection >= count)
		{
			return;
		}

		if (OptionValues.GetTextValue(mValues, 0).Length() == 0)
		{
			mCVar.SetFloat(OptionValues.GetValue(mValues, selection));
		}
		else
		{
			mCVar.SetString(OptionValues.GetTextValue(mValues, selection));
		}
	}
}

// Explicit language selector. MENUDEF keyword: NakaraLanguageDropDown.
// Existing NakaraDropDown items bound to the language CVar also work, but
// this class is useful when the language selector uses a differently named CVar.
class OptionMenuItemNakaraLanguageDropDown : OptionMenuItemNakaraDropDown
{
	OptionMenuItemNakaraLanguageDropDown Init(String label, Name command, Name values,
		CVar graycheck = null, int center = 0, int graycheckVal = 0)
	{
		Super.Init(label, command, values, graycheck, center, graycheckVal);
		return self;
	}

	override Font GetDropDownItemFont(int selection)
	{
		return GetLanguageEntryFont(selection);
	}
}

// Resolution remains a specialized drop-down because selecting one entry
// changes several video CVARs through menu_resolution_preset.
class OptionMenuItemNakaraResolution : OptionMenuItemNakaraDropDownBase
{
	CVar mPreset;
	CVar mScaleMode;
	CVar mCustomWidth;
	CVar mCustomHeight;

	OptionMenuItemNakaraResolution Init(String label, Name values)
	{
		InitDropDown(label, 'menu_resolution_preset', values);
		mPreset = CVar.FindCVar('menu_resolution_preset');
		mScaleMode = CVar.FindCVar('vid_scalemode');
		mCustomWidth = CVar.FindCVar('vid_scale_customwidth');
		mCustomHeight = CVar.FindCVar('vid_scale_customheight');
		return self;
	}

	override int GetSelection()
	{
		int width = screen.GetWidth();
		int height = screen.GetHeight();

		if (mScaleMode && mScaleMode.GetInt() == 5 && mCustomWidth && mCustomHeight)
		{
			width = mCustomWidth.GetInt();
			height = mCustomHeight.GetInt();
		}

		int count = OptionValues.GetCount(mValues);
		for (int i = 0; i < count; i++)
		{
			int packed = int(OptionValues.GetValue(mValues, i));
			if (packed / 10000 == width && packed % 10000 == height)
			{
				return i;
			}
		}
		return -1;
	}

	override void SetSelection(int selection)
	{
		int count = OptionValues.GetCount(mValues);
		if (!mPreset || selection < 0 || selection >= count)
		{
			return;
		}

		// Reset first so re-selecting the same preset still invokes the callback.
		mPreset.SetInt(0);
		mPreset.SetInt(int(OptionValues.GetValue(mValues, selection)));
	}
}

class NakaraDropDownMenu : OptionMenu
{
	OptionMenuItemNakaraDropDownBase ActiveDropDown()
	{
		for (int i = 0; i < mDesc.mItems.Size(); i++)
		{
			let item = OptionMenuItemNakaraDropDownBase(mDesc.mItems[i]);
			if (item && item.IsDropDownOpen()) return item;
		}
		return null;
	}

	void CloseAllDropDowns(bool animate = false)
	{
		for (int i = 0; i < mDesc.mItems.Size(); i++)
		{
			let item = OptionMenuItemNakaraDropDownBase(mDesc.mItems[i]);
			if (item) item.CloseDropDown(animate);
		}
	}

	override void Init(Menu parent, OptionMenuDescriptor desc)
	{
		Super.Init(parent, desc);
		CloseAllDropDowns();
	}

	override void OnDestroy()
	{
		CloseAllDropDowns();
		Super.OnDestroy();
	}

	override bool MenuEvent(int mkey, bool fromcontroller)
	{
		let item = ActiveDropDown();
		if (item)
		{
			return item.HandleDropDownKey(mkey, fromcontroller);
		}
		return Super.MenuEvent(mkey, fromcontroller);
	}

	override bool OnUIEvent(UIEvent ev)
	{
		let item = ActiveDropDown();
		if (item)
		{
			if (ev.type == UIEvent.Type_WheelUp) return item.HandleDropDownWheel(-1);
			if (ev.type == UIEvent.Type_WheelDown) return item.HandleDropDownWheel(1);
			if (ev.type == UIEvent.Type_Char) return true;
		}
		return Super.OnUIEvent(ev);
	}

	override bool MouseEvent(int type, int x, int y)
	{
		let item = ActiveDropDown();
		if (item)
		{
			return item.HandleDropDownMouse(type, x, y);
		}
		return Super.MouseEvent(type, x, y);
	}

	override void Drawer()
	{
		Super.Drawer();
		let item = ActiveDropDown();
		if (item) item.DrawDropDown(mDesc);
	}
}

class GameplayMenu : NakaraDropDownMenu
{
	override void Drawer ()
	{
		Super.Drawer();

		String s = String.Format("dmflags = %d  dmflags2 = %d  dmflags3 = %d", dmflags, dmflags2, dmflags3);
		screen.DrawText (OptionFont(), OptionMenuSettings.mFontColorValue,
			(screen.GetWidth() - OptionWidth (s) * CleanXfac_1) / 2, 35 * CleanXfac_1, s,
			DTA_CleanNoMove_1, true);
	}
}

class CompatibilityMenu : NakaraDropDownMenu
{
	override void Drawer ()
	{
		Super.Drawer();

		String s = String.Format("compatflags = %d  compatflags2 = %d", compatflags, compatflags2);
		screen.DrawText (OptionFont(), OptionMenuSettings.mFontColorValue,
			(screen.GetWidth() - OptionWidth (s) * CleanXfac_1) / 2, 35 * CleanXfac_1, s,
			DTA_CleanNoMove_1, true);
	}
}

//=============================================================================
//
// Placeholder classes for overhauled video mode menu. Do not use!
// Their sole purpose is to support mods with full copy of embedded MENUDEF
//
//=============================================================================

class OptionMenuItemScreenResolution : OptionMenuItem
{
	String mResTexts[3];
	int mSelection;
	int mHighlight;
	int mMaxValid;

	enum EValues
	{
		SRL_INDEX = 0x30000,
		SRL_SELECTION = 0x30003,
		SRL_HIGHLIGHT = 0x30004,
	};

	OptionMenuItemScreenResolution Init(String command)
	{
		return self;
	}

	override bool Selectable()
	{
		return false;
	}
}

class VideoModeMenu : NakaraDropDownMenu
{
	static bool SetSelectedSize()
	{
		return false;
	}
}

class DoomMenuDelegate : MenuDelegateBase
{
	override void PlaySound(Name snd)
	{
		String s = snd;
		S_StartSound (s, CHAN_VOICE, CHANF_UI, snd_menuvolume); 	
	}
} 
