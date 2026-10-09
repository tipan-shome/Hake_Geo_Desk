/***************************************************************************
  qgsappribbon.cpp
  -------------------
  begin                : August 2026
  copyright            : (C) 2026 by Hake Technologies
 ***************************************************************************/

/***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsappribbon.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include "qgisapp.h"
#include "qgsapplication.h"
#include "qgsfloatingwidget.h"
#include "qgslocatorwidget.h"

#include <QAction>
#include <QApplication>
#include <QDockWidget>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QPainter>
#include <QPixmap>
#include <QPointer>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSizePolicy>
#include <QString>
#include <QStyle>
#include <QStyleOption>
#include <QStylePainter>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWidget>
#include <QWidgetAction>

#include "moc_qgsappribbon.cpp"

using namespace Qt::StringLiterals;

namespace
{
  QFont ribbonCaptionFont( const QFont &base )
  {
    QFont f = base;
    if ( f.pointSizeF() > 0 )
      f.setPointSizeF( f.pointSizeF() * 0.85 );
    else
      f.setPixelSize( std::max( 8, static_cast<int>( std::round( f.pixelSize() * 0.85 ) ) ) );
    f.setWeight( QFont::Medium );
    return f;
  }

  bool isPresentableAction( const QAction *action )
  {
    return action && !action->isSeparator() && !qobject_cast<const QWidgetAction *>( action );
  }

  //! The menu behind a QMenu::menuAction() drop-down, as opposed to a regular command that merely carries a menu.
  QMenu *dropDownMenu( const QAction *action )
  {
    QMenu *menu = action ? action->menu() : nullptr;
    return menu && menu->menuAction() == action ? menu : nullptr;
  }

  //! Visible, and for a menu drop-down, the menu has at least one visible command.
  bool isShowableCommand( const QAction *action )
  {
    if ( !action || !action->isVisible() )
      return false;
    if ( const QMenu *menu = dropDownMenu( action ) )
    {
      const QList<QAction *> actions = menu->actions();
      return std::any_of( actions.cbegin(), actions.cend(), []( const QAction *a ) { return !a->isSeparator() && a->isVisible(); } );
    }
    return true;
  }

  /**
   * Ribbon commands show only their icon; the action's tooltip (which QAction derives from
   * its text when unset) is the description. Commands without an icon or any description keep
   * their label so they never become an unidentifiable blank or unexplained button.
   */
  bool presentsIconOnly( const QAction *action )
  {
    return action && !action->icon().isNull() && !action->toolTip().isEmpty();
  }

  //! Toolbar widgets such as drop-down tool buttons are presented through their default action.
  QAction *presentableToolbarAction( QAction *action )
  {
    if ( auto *widgetAction = qobject_cast<QWidgetAction *>( action ) )
    {
      auto *button = qobject_cast<QToolButton *>( widgetAction->defaultWidget() );
      return button ? button->defaultAction() : nullptr;
    }
    return action;
  }
} // namespace

// Group caption that elides instead of forcing its group wider than its buttons.
// Color comes from the theme QSS (#HakeAppRibbonGroupCaption) via the palette.
class QgsAppRibbonCaption : public QWidget
{
  public:
    QgsAppRibbonCaption( const QString &text, QWidget *parent )
      : QWidget( parent )
      , mText( text )
    {
      setObjectName( u"HakeAppRibbonGroupCaption"_s );
      setSizePolicy( QSizePolicy::Ignored, QSizePolicy::Fixed );
      setAttribute( Qt::WA_TransparentForMouseEvents, true );
    }

    QSize sizeHint() const override
    {
      const QFontMetrics fm( font() );
      return QSize( fm.horizontalAdvance( mText ) + fm.averageCharWidth() * 2, fm.height() );
    }

    QSize minimumSizeHint() const override { return QSize( 0, QFontMetrics( font() ).height() ); }

  protected:
    void paintEvent( QPaintEvent * ) override
    {
      QPainter p( this );
      p.setPen( palette().color( QPalette::WindowText ) );
      const QString shown = fontMetrics().elidedText( mText, Qt::ElideRight, width() );
      p.drawText( rect(), Qt::AlignHCenter | Qt::AlignVCenter, shown );
    }

  private:
    QString mText;
};

/**
 * The one command button used everywhere in the ribbon: a fixed square cell with the icon
 * centred in it, plus a fixed caret strip for menu drop-downs. Only the icon is painted; the
 * button keeps the action's text and tooltip for accessibility and hover help. Commands that
 * cannot be shown icon-only paint their label instead, at the same height and centre line.
 * Size depends only on the shared metrics, icon and label, never on enabled/checked/hover
 * state, and painting never offsets the icon, so the ribbon does not move when actions change state.
 * Styled through #HakeAppRibbonButton in the theme QSS.
 */
class QgsAppRibbonButton : public QToolButton
{
  public:
    QgsAppRibbonButton( QWidget *parent, const QgsAppRibbonMetrics &metrics )
      : QToolButton( parent )
      , mCell( metrics.cellSize )
      , mCaret( metrics.caretWidth )
      , mInset( metrics.spaceSm )
      , mGap( metrics.spaceXs )
      , mLabelMax( metrics.labelMax )
    {
      setObjectName( u"HakeAppRibbonButton"_s );
      setAutoRaise( true );
      // Reachable by keyboard, but a mouse click must not steal focus from the map canvas.
      setFocusPolicy( Qt::TabFocus );
      setToolButtonStyle( Qt::ToolButtonIconOnly );
      setIconSize( QSize( metrics.cellIcon, metrics.cellIcon ) );
      setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Fixed );
      setFixedHeight( mCell );
    }

    //! Shown instead of the action text; the action itself is not modified.
    void setLabel( const QString &label ) { mLabel = label; }

    void setDropDown( bool dropDown )
    {
      mDropDown = dropDown;
      if ( dropDown )
        setPopupMode( QToolButton::InstantPopup );
    }

    //! Shows only the icon; otherwise the label (beside the icon, if any) is painted.
    void setIconOnly( bool iconOnly )
    {
      mIconOnly = iconOnly;
      updateGeometry();
    }

    QSize sizeHint() const override
    {
      const int body = mIconOnly ? mCell : labelledWidth();
      return QSize( body + ( mDropDown ? mCaret : 0 ), mCell );
    }

    QSize minimumSizeHint() const override { return sizeHint(); }

  protected:
    void paintEvent( QPaintEvent * ) override
    {
      QStylePainter p( this );
      QStyleOptionToolButton opt;
      initStyleOption( &opt );

      // Panel only (background, border and state from the theme QSS); content is placed below
      // so icons sit on the same centre line in every state.
      QStyleOptionToolButton panel = opt;
      panel.text.clear();
      panel.icon = QIcon();
      panel.features &= ~( QStyleOptionToolButton::HasMenu | QStyleOptionToolButton::MenuButtonPopup );
      p.drawComplexControl( QStyle::CC_ToolButton, panel );

      QIcon::Mode mode = QIcon::Normal;
      if ( !isEnabled() )
        mode = QIcon::Disabled;
      else if ( ( opt.state & QStyle::State_MouseOver ) && ( opt.state & QStyle::State_AutoRaise ) )
        mode = QIcon::Active;
      const QIcon::State state = isChecked() ? QIcon::On : QIcon::Off;
      const QColor textColor = palette().color( isEnabled() ? QPalette::Normal : QPalette::Disabled, QPalette::ButtonText );
      const QRect body = QStyle::visualRect( layoutDirection(), rect(), QRect( 0, 0, width() - ( mDropDown ? mCaret : 0 ), height() ) );

      if ( mIconOnly )
      {
        const QRect iconRect = QStyle::alignedRect( layoutDirection(), Qt::AlignCenter, iconSize(), body );
        icon().paint( &p, iconRect, Qt::AlignCenter, mode, state );
      }
      else
      {
        int x = mInset;
        if ( !icon().isNull() )
        {
          const QRect iconRect = QStyle::visualRect( layoutDirection(), rect(), QRect( QPoint( x, ( height() - iconSize().height() ) / 2 ), iconSize() ) );
          icon().paint( &p, iconRect, Qt::AlignCenter, mode, state );
          x += iconSize().width() + mGap;
        }
        const QRect textRect = QStyle::visualRect( layoutDirection(), rect(), QRect( x, 0, std::max( 0, body.width() - x - mInset ), height() ) );
        p.setPen( textColor );
        p.drawText( textRect, Qt::AlignVCenter | Qt::AlignLeft | Qt::TextHideMnemonic, shownLabel() );
      }

      if ( mDropDown )
      {
        const QRect caretRect = QStyle::visualRect( layoutDirection(), rect(), QRect( width() - mCaret, 0, mCaret, height() ) );
        p.setPen( textColor );
        p.drawText( caretRect, Qt::AlignCenter, u"\u25BE"_s );
      }
    }

  private:
    QString shownLabel() const
    {
      const QString text = mLabel.isEmpty() ? this->text() : mLabel;
      return fontMetrics().elidedText( text, Qt::ElideRight, mLabelMax, Qt::TextShowMnemonic );
    }

    int labelledWidth() const
    {
      int w = 2 * mInset + fontMetrics().size( Qt::TextShowMnemonic, shownLabel() ).width();
      if ( !icon().isNull() )
        w += iconSize().width() + mGap;
      return std::max( w, mCell );
    }

    int mCell = 0;
    int mCaret = 0;
    int mInset = 0;
    int mGap = 0;
    int mLabelMax = 0;
    QString mLabel;
    bool mDropDown = false;
    bool mIconOnly = false;
};

//! Subtle vertical group separator, inset from the page edges; color from the theme QSS via the palette.
class QgsAppRibbonSeparator : public QWidget
{
  public:
    explicit QgsAppRibbonSeparator( QWidget *parent )
      : QWidget( parent )
    {
      setObjectName( u"HakeAppRibbonSeparator"_s );
      setFixedWidth( 1 );
      setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Expanding );
      setAttribute( Qt::WA_TransparentForMouseEvents, true );
    }

    void setInsets( int top, int bottom )
    {
      mTop = top;
      mBottom = bottom;
      update();
    }

  protected:
    void paintEvent( QPaintEvent * ) override
    {
      QPainter p( this );
      p.fillRect( QRect( 0, mTop, width(), std::max( 0, height() - mTop - mBottom ) ), palette().color( QPalette::WindowText ) );
    }

  private:
    int mTop = 0;
    int mBottom = 0;
};

class QgsAppRibbonGroup : public QWidget
{
  public:
    enum Level
    {
      Full,
      Compact,
      IconsOnly,
      Collapsed,
    };

    struct Entry
    {
        QPointer<QAction> action;
        bool primary = false;
        //! Dock whose toggleViewAction() fills this entry once the dock exists
        QString dockObjectName;
        //! Menu whose menuAction() fills this entry once the menu exists
        QString menuObjectName;
        //! Button label overriding the action text for presentation only
        QString label;
    };

    QgsAppRibbonGroup( const QString &title, QWidget *parent );

    QString title() const { return mTitle; }
    void addEntry( const Entry &entry );
    void setEntries( const QList<Entry> &entries );
    bool resolveDeferred( QObject *root );
    QList<QAction *> commands() const;
    bool hasCommands() const { return !commands().isEmpty(); }

    void setMetrics( const QgsAppRibbonMetrics &metrics );
    void rebuild();
    void scheduleRebuild();

    Level level() const { return mLevel; }
    void setLevel( Level level );
    int widthForLevel( Level level ) const;

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    bool eventFilter( QObject *watched, QEvent *event ) override;

  private:
    void watchAction( QAction *action );
    void actionChanged();
    QWidget *buildVariant( Level level );
    QgsAppRibbonButton *makeButton( QWidget *parent, const Entry &entry ) const;

    QString mTitle;
    QList<Entry> mEntries;
    QgsAppRibbonMetrics mMetrics;
    Level mLevel = Full;
    QVBoxLayout *mLayout = nullptr;
    QWidget *mContent = nullptr;
    QHBoxLayout *mContentLayout = nullptr;
    QgsAppRibbonCaption *mCaption = nullptr;
    std::array<QWidget *, 4> mVariants { { nullptr, nullptr, nullptr, nullptr } };
    //! Icon-only decision each action was last built with
    QHash<const QAction *, bool> mBuiltIconOnly;
    bool mRebuildPending = false;
};

class QgsAppRibbonPage : public QWidget
{
  public:
    explicit QgsAppRibbonPage( QWidget *parent );

    QgsAppRibbonGroup *addGroup( const QString &title );
    const QList<QgsAppRibbonGroup *> &groups() const { return mGroups; }
    void setMetrics( const QgsAppRibbonMetrics &metrics );
    void relayout();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

  protected:
    void resizeEvent( QResizeEvent *event ) override;
    void showEvent( QShowEvent *event ) override;

  private:
    int requiredWidth( const QVector<QgsAppRibbonGroup::Level> &levels, const QVector<bool> &hidden, bool overflow ) const;

    QHBoxLayout *mLayout = nullptr;
    QList<QgsAppRibbonGroup *> mGroups;
    QList<QgsAppRibbonSeparator *> mSeparators;
    QToolButton *mOverflow = nullptr;
    QMenu *mOverflowMenu = nullptr;
    bool mInRelayout = false;
};

//
// QgsAppRibbonGroup
//

QgsAppRibbonGroup::QgsAppRibbonGroup( const QString &title, QWidget *parent )
  : QWidget( parent )
  , mTitle( title )
{
  setObjectName( u"HakeAppRibbonGroup"_s );
  setAccessibleName( title );
  setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Expanding );

  mLayout = new QVBoxLayout( this );
  mLayout->setContentsMargins( 0, 0, 0, 0 );
  mLayout->setSpacing( 0 );

  mContent = new QWidget( this );
  mContentLayout = new QHBoxLayout( mContent );
  mContentLayout->setContentsMargins( 0, 0, 0, 0 );
  mContentLayout->setSpacing( 0 );
  mLayout->addWidget( mContent, 0 );

  mCaption = new QgsAppRibbonCaption( title, this );
  mLayout->addWidget( mCaption, 0 );
  mLayout->addStretch( 1 );
}

void QgsAppRibbonGroup::watchAction( QAction *action )
{
  if ( !action )
    return;
  connect( action, &QAction::visibleChanged, this, &QgsAppRibbonGroup::scheduleRebuild, Qt::UniqueConnection );
  // Icons can arrive after the ribbon is built (plugin menus, theme switches).
  connect( action, &QAction::changed, this, &QgsAppRibbonGroup::actionChanged, Qt::UniqueConnection );
  // Menu drop-downs appear/disappear as plugins populate or empty the menu.
  if ( QMenu *menu = dropDownMenu( action ) )
    menu->installEventFilter( this );
}

void QgsAppRibbonGroup::actionChanged()
{
  // Enabled/checked changes are frequent; only a change of presentation needs a rebuild.
  const auto *action = qobject_cast<const QAction *>( sender() );
  const auto it = mBuiltIconOnly.constFind( action );
  if ( it != mBuiltIconOnly.constEnd() && *it != presentsIconOnly( action ) )
    scheduleRebuild();
}

bool QgsAppRibbonGroup::eventFilter( QObject *watched, QEvent *event )
{
  if ( ( event->type() == QEvent::ActionAdded || event->type() == QEvent::ActionRemoved ) && qobject_cast<QMenu *>( watched ) )
    scheduleRebuild();
  return QWidget::eventFilter( watched, event );
}

void QgsAppRibbonGroup::addEntry( const Entry &entry )
{
  if ( entry.action && !isPresentableAction( entry.action ) )
    return;
  if ( !entry.action && entry.dockObjectName.isEmpty() && entry.menuObjectName.isEmpty() )
    return;
  mEntries.append( entry );
  watchAction( entry.action );
}

void QgsAppRibbonGroup::setEntries( const QList<Entry> &entries )
{
  mEntries.clear();
  for ( const Entry &entry : entries )
    addEntry( entry );
  scheduleRebuild();
}

bool QgsAppRibbonGroup::resolveDeferred( QObject *root )
{
  bool resolved = false;
  for ( Entry &entry : mEntries )
  {
    if ( entry.action )
      continue;
    if ( !entry.dockObjectName.isEmpty() )
    {
      if ( QDockWidget *dock = root->findChild<QDockWidget *>( entry.dockObjectName ) )
        entry.action = dock->toggleViewAction();
    }
    else if ( !entry.menuObjectName.isEmpty() )
    {
      if ( QMenu *menu = root->findChild<QMenu *>( entry.menuObjectName ) )
        entry.action = menu->menuAction();
    }
    if ( entry.action )
    {
      watchAction( entry.action );
      resolved = true;
    }
  }
  return resolved;
}

QList<QAction *> QgsAppRibbonGroup::commands() const
{
  QList<QAction *> list;
  for ( const Entry &entry : mEntries )
  {
    if ( isShowableCommand( entry.action ) )
      list << entry.action;
  }
  return list;
}

void QgsAppRibbonGroup::setMetrics( const QgsAppRibbonMetrics &metrics )
{
  mMetrics = metrics;
  // Every group shares the same command-area height and caption row, so command rows
  // and captions sit on the same baselines on every tab.
  mLayout->setContentsMargins( metrics.spaceSm, 0, metrics.spaceSm, 0 );
  mLayout->setSpacing( metrics.spaceXs );
  mContent->setFixedHeight( metrics.buttonAreaHeight );
  mCaption->setFixedHeight( metrics.captionHeight );
  rebuild();
}

void QgsAppRibbonGroup::scheduleRebuild()
{
  if ( mRebuildPending )
    return;
  mRebuildPending = true;
  QTimer::singleShot( 0, this, [this] {
    mRebuildPending = false;
    rebuild();
    if ( auto *page = dynamic_cast<QgsAppRibbonPage *>( parentWidget() ) )
      page->relayout();
  } );
}

void QgsAppRibbonGroup::rebuild()
{
  // deleteLater: a rebuild can be triggered while one of these buttons is still
  // inside its own click handler (e.g. an action that changes the theme).
  for ( QWidget *&variant : mVariants )
  {
    if ( variant )
    {
      variant->hide();
      variant->deleteLater();
      variant = nullptr;
    }
  }

  mBuiltIconOnly.clear();
  for ( const Entry &entry : std::as_const( mEntries ) )
  {
    if ( entry.action )
      mBuiltIconOnly.insert( entry.action, presentsIconOnly( entry.action ) );
  }

  mCaption->setFont( ribbonCaptionFont( font() ) );
  for ( int l = Full; l <= Collapsed; ++l )
  {
    mVariants[l] = buildVariant( static_cast<Level>( l ) );
    mContentLayout->addWidget( mVariants[l], 0, Qt::AlignLeft | Qt::AlignTop );
    mVariants[l]->ensurePolished();
  }
  setLevel( mLevel );
}

QgsAppRibbonButton *QgsAppRibbonGroup::makeButton( QWidget *parent, const Entry &entry ) const
{
  auto *button = new QgsAppRibbonButton( parent, mMetrics );
  button->setDefaultAction( entry.action );
  button->setLabel( entry.label );
  button->setDropDown( dropDownMenu( entry.action ) != nullptr );
  button->setIconOnly( presentsIconOnly( entry.action ) );
  return button;
}

QWidget *QgsAppRibbonGroup::buildVariant( Level level )
{
  const QgsAppRibbonMetrics &m = mMetrics;

  // One row of equal cells, vertically centred in the shared command area, so every icon on
  // every tab sits on the same centre line.
  auto *variant = new QWidget( mContent );
  variant->setFixedHeight( m.buttonAreaHeight );
  auto *row = new QHBoxLayout( variant );
  row->setContentsMargins( 0, 0, 0, 0 );
  row->setSpacing( m.spaceXs );

  QList<Entry> visibleEntries;
  for ( const Entry &entry : std::as_const( mEntries ) )
  {
    if ( isShowableCommand( entry.action ) )
      visibleEntries << entry;
  }
  if ( visibleEntries.isEmpty() )
    return variant;

  if ( level == Collapsed )
  {
    // The whole group behind one drop-down cell; the title is its tooltip and accessible name.
    auto *button = new QgsAppRibbonButton( variant, m );
    button->setText( mTitle );
    button->setToolTip( mTitle );
    button->setAccessibleName( mTitle );
    button->setIcon( visibleEntries.first().action->icon() );
    button->setDropDown( true );
    button->setIconOnly( !button->icon().isNull() );
    auto *menu = new QMenu( button );
    menu->addActions( commands() );
    button->setMenu( menu );
    row->addWidget( button, 0, Qt::AlignLeft | Qt::AlignVCenter );
    return variant;
  }

  for ( const Entry &entry : std::as_const( visibleEntries ) )
    row->addWidget( makeButton( variant, entry ), 0, Qt::AlignVCenter );
  return variant;
}

void QgsAppRibbonGroup::setLevel( Level level )
{
  mLevel = level;
  for ( int l = Full; l <= Collapsed; ++l )
  {
    if ( mVariants[l] )
      mVariants[l]->setVisible( l == level );
  }
  mCaption->setVisible( mMetrics.captions && level != Collapsed );
  updateGeometry();
}

int QgsAppRibbonGroup::widthForLevel( Level level ) const
{
  const QWidget *variant = mVariants[level];
  if ( !variant )
    return 0;
  const int padding = 2 * mMetrics.spaceSm;
  const int content = variant->sizeHint().width();
  if ( !mMetrics.captions || level == Collapsed )
    return content + padding;
  const int caption = mCaption->sizeHint().width();
  if ( level == IconsOnly )
    return std::max( content, std::min( caption, mCaption->fontMetrics().averageCharWidth() * 6 ) ) + padding;
  return std::max( content, caption ) + padding;
}

QSize QgsAppRibbonGroup::sizeHint() const
{
  return QSize( widthForLevel( mLevel ), QWidget::sizeHint().height() );
}

QSize QgsAppRibbonGroup::minimumSizeHint() const
{
  return QSize( widthForLevel( mLevel ), 0 );
}

//
// QgsAppRibbonPage
//

QgsAppRibbonPage::QgsAppRibbonPage( QWidget *parent )
  : QWidget( parent )
{
  setObjectName( u"HakeAppRibbonPage"_s );
  setAttribute( Qt::WA_StyledBackground, true );
  setSizePolicy( QSizePolicy::Ignored, QSizePolicy::Ignored );

  mLayout = new QHBoxLayout( this );
  // Gaps between groups come from each group's own padding, so separators sit evenly between them.
  mLayout->setSpacing( 0 );
  mLayout->addStretch( 1 );

  mOverflowMenu = new QMenu( this );
  mOverflow = new QToolButton( this );
  mOverflow->setText( u"\u00BB"_s );
  mOverflow->setToolTip( QObject::tr( "More commands" ) );
  mOverflow->setAccessibleName( QObject::tr( "More commands" ) );
  mOverflow->setToolButtonStyle( Qt::ToolButtonTextOnly );
  mOverflow->setAutoRaise( true );
  mOverflow->setFocusPolicy( Qt::TabFocus );
  mOverflow->setPopupMode( QToolButton::InstantPopup );
  mOverflow->setMenu( mOverflowMenu );
  mOverflow->hide();
  mLayout->addWidget( mOverflow, 0, Qt::AlignVCenter );
}

QgsAppRibbonGroup *QgsAppRibbonPage::addGroup( const QString &title )
{
  auto *group = new QgsAppRibbonGroup( title, this );
  auto *separator = new QgsAppRibbonSeparator( this );

  // Keep the trailing stretch and overflow button at the end.
  const int insertAt = mLayout->count() - 2;
  mLayout->insertWidget( insertAt, group );
  mLayout->insertWidget( insertAt + 1, separator );
  mGroups << group;
  mSeparators << separator;
  return group;
}

void QgsAppRibbonPage::setMetrics( const QgsAppRibbonMetrics &metrics )
{
  // +1 bottom margin reserves the page's 1px bottom border drawn by the theme.
  mLayout->setContentsMargins( metrics.spaceMd, metrics.spaceSm, metrics.spaceMd, metrics.spaceXs + 1 );
  mOverflow->setFixedHeight( metrics.cellSize );
  for ( QgsAppRibbonSeparator *separator : std::as_const( mSeparators ) )
    separator->setInsets( metrics.spaceSm, metrics.spaceXs );
  for ( QgsAppRibbonGroup *group : std::as_const( mGroups ) )
    group->setMetrics( metrics );
  relayout();
}

QSize QgsAppRibbonPage::sizeHint() const
{
  const QVector<QgsAppRibbonGroup::Level> levels( mGroups.size(), QgsAppRibbonGroup::Full );
  const QVector<bool> hidden( mGroups.size(), false );
  return QSize( requiredWidth( levels, hidden, false ), 0 );
}

QSize QgsAppRibbonPage::minimumSizeHint() const
{
  // Height is owned by QgsAppRibbon; width always adapts through relayout().
  return QSize( 0, 0 );
}

int QgsAppRibbonPage::requiredWidth( const QVector<QgsAppRibbonGroup::Level> &levels, const QVector<bool> &hidden, bool overflow ) const
{
  const QMargins margins = mLayout->contentsMargins();
  int width = margins.left() + margins.right();
  int items = 0;
  int visibleGroups = 0;
  for ( int i = 0; i < mGroups.size(); ++i )
  {
    if ( hidden[i] || !mGroups[i]->hasCommands() )
      continue;
    width += mGroups[i]->widthForLevel( levels[i] );
    ++visibleGroups;
    ++items;
  }
  if ( visibleGroups > 1 )
  {
    width += ( visibleGroups - 1 ) * mSeparators.first()->minimumWidth();
    items += visibleGroups - 1;
  }
  if ( overflow )
  {
    width += mOverflow->sizeHint().width();
    ++items;
  }
  if ( items > 1 )
    width += ( items - 1 ) * mLayout->spacing();
  return width;
}

void QgsAppRibbonPage::relayout()
{
  if ( mInRelayout || mGroups.isEmpty() )
    return;
  mInRelayout = true;

  const int available = width();
  const int n = static_cast<int>( mGroups.size() );
  QVector<QgsAppRibbonGroup::Level> levels( n, QgsAppRibbonGroup::Full );
  QVector<bool> hidden( n, false );
  bool overflow = false;

  auto fits = [&] { return requiredWidth( levels, hidden, overflow ) <= available; };

  // Compress one level at a time, rightmost group first, so the most-used
  // commands on the left stay expanded longest.
  bool done = fits();
  for ( int level = QgsAppRibbonGroup::Compact; !done && level <= QgsAppRibbonGroup::Collapsed; ++level )
  {
    for ( int i = n - 1; !done && i >= 0; --i )
    {
      // A collapsed drop-down can be as wide as a small group's expanded
      // layout; only collapse when it saves space.
      const bool saves = level != QgsAppRibbonGroup::Collapsed || mGroups[i]->widthForLevel( QgsAppRibbonGroup::Collapsed ) < mGroups[i]->widthForLevel( levels[i] );
      if ( levels[i] < level && saves )
      {
        levels[i] = static_cast<QgsAppRibbonGroup::Level>( level );
        done = fits();
      }
    }
  }
  if ( !done )
  {
    overflow = true;
    for ( int i = n - 1; i >= 0 && !fits(); --i )
      hidden[i] = true;
  }

  const QList<QMenu *> oldSubMenus = mOverflowMenu->findChildren<QMenu *>( Qt::FindDirectChildrenOnly );
  mOverflowMenu->clear();
  for ( QMenu *subMenu : oldSubMenus )
    subMenu->deleteLater();

  int lastVisible = -1;
  for ( int i = 0; i < n; ++i )
  {
    QgsAppRibbonGroup *group = mGroups[i];
    const bool hasCommands = group->hasCommands();
    group->setLevel( levels[i] );
    group->setVisible( hasCommands && !hidden[i] );
    if ( !hasCommands )
      continue;
    if ( hidden[i] )
    {
      QMenu *subMenu = mOverflowMenu->addMenu( group->title() );
      subMenu->addActions( group->commands() );
    }
    else
    {
      lastVisible = i;
    }
  }
  for ( int i = 0; i < n; ++i )
    mSeparators[i]->setVisible( mGroups[i]->isVisibleTo( this ) && i < lastVisible );
  mOverflow->setVisible( overflow );

  mInRelayout = false;
}

void QgsAppRibbonPage::resizeEvent( QResizeEvent *event )
{
  QWidget::resizeEvent( event );
  if ( event->size().width() != event->oldSize().width() )
    relayout();
}

void QgsAppRibbonPage::showEvent( QShowEvent *event )
{
  QWidget::showEvent( event );
  relayout();
}

//
// QgsAppRibbon
//

QgsAppRibbon::QgsAppRibbon( QWidget *parent, QgisApp *app )
  : QTabWidget( parent )
  , mApp( app )
{
  setObjectName( u"HakeAppRibbon"_s );
  setDocumentMode( true );
  setMovable( false );
  setUsesScrollButtons( true );
  // Height comes from measured metrics (sizeHint), never from a fixed pixel value.
  setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Fixed );
  // Fusion leaves the empty region after the last tab unpainted unless QSS backgrounds are forced.
  // WA_StyledBackground is Qt-portable (Wayland-safe); do not use platform window APIs here.
  setAttribute( Qt::WA_StyledBackground, true );
  tabBar()->setAttribute( Qt::WA_StyledBackground, true );
  tabBar()->setAutoFillBackground( true );
  tabBar()->setExpanding( false );
  tabBar()->setDrawBase( false );
  tabBar()->setElideMode( Qt::ElideNone );
  tabBar()->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Preferred );

  // Brand at the right end of the tab strip; the tab bar is sized to end where it begins.
  auto *tabFiller = new QWidget( this );
  tabFiller->setObjectName( u"HakeAppRibbonTabFiller"_s );
  tabFiller->setAttribute( Qt::WA_StyledBackground, true );
  tabFiller->setSizePolicy( QSizePolicy::Expanding, QSizePolicy::Preferred );
  auto *fillerLayout = new QHBoxLayout( tabFiller );
  fillerLayout->setContentsMargins( 0, 0, 0, 0 );
  mBrand = new QLabel( u"HAKE GEOSPATIAL"_s, tabFiller );
  mBrand->setObjectName( u"HakeAppRibbonBrand"_s );
  mBrand->setAccessibleName( tr( "Hake Geospatial" ) );
  fillerLayout->addWidget( mBrand, 0, Qt::AlignVCenter );
  setCornerWidget( tabFiller, Qt::TopRightCorner );

  if ( !mApp )
    return;

  // The ribbon tab strip is the only navigation row: exactly these nine tabs, in this order.
  // Classic menus (Project, Edit, View, Layer, Settings, Plugins, ...) are not tabs; they are
  // exposed as drop-downs inside the matching tab.

  // Home: project, editing, navigation and application settings
  {
    QgsAppRibbonPage *page = addPage( tr( "Home" ) );
    QgsAppRibbonGroup *project = addGroup( page, tr( "Project" ) );
    addNamedAction( project, u"mActionNewProject"_s, true );
    addNamedAction( project, u"mActionOpenProject"_s, true );
    addNamedAction( project, u"mActionSaveProject"_s, true );
    addNamedAction( project, u"mActionSaveProjectAs"_s );
    addNamedAction( project, u"mActionProjectProperties"_s );
    addNamedAction( project, u"mActionExit"_s );
    addMenu( project, mApp->projectMenu() );

    QgsAppRibbonGroup *edit = addGroup( page, tr( "Editing" ) );
    addNamedAction( edit, u"mActionUndo"_s );
    addNamedAction( edit, u"mActionRedo"_s );
    addMenu( edit, mApp->editMenu() );

    QgsAppRibbonGroup *navigation = addGroup( page, tr( "Navigation" ) );
    addNamedAction( navigation, u"mActionPan"_s, true );
    addNamedAction( navigation, u"mActionZoomIn"_s );
    addNamedAction( navigation, u"mActionZoomOut"_s );
    addNamedAction( navigation, u"mActionZoomFullExtent"_s );
    addNamedAction( navigation, u"mActionDraw"_s );

    QgsAppRibbonGroup *identify = addGroup( page, tr( "Identify" ) );
    addNamedAction( identify, u"mActionIdentify"_s, true );
    addNamedAction( identify, u"mActionOpenTable"_s );

    QgsAppRibbonGroup *application = addGroup( page, tr( "Application" ) );
    addNamedAction( application, u"mActionOptions"_s, true );
    addNamedAction( application, u"mActionStyleManager"_s );
    addNamedAction( application, u"mActionCustomProjection"_s );
    addMenu( application, mApp->settingsMenu() );
  }

  // Data: layer sources, layouts, database and web services
  {
    QgsAppRibbonPage *page = addPage( tr( "Data" ) );
    QgsAppRibbonGroup *layers = addGroup( page, tr( "Layers" ) );
    addNamedAction( layers, u"mActionDataSourceManager"_s, true );
    addNamedAction( layers, u"mActionAddOgrLayer"_s );
    addNamedAction( layers, u"mActionAddRasterLayer"_s );
    addNamedAction( layers, u"mActionAddMeshLayer"_s );
    addNamedAction( layers, u"mActionAddDelimitedText"_s );
    addNamedAction( layers, u"mActionAddSpatiaLiteLayer"_s );
    addNamedAction( layers, u"mActionAddVirtualLayer"_s );
    addNamedAction( layers, u"mActionAddWmsLayer"_s );
    addNamedAction( layers, u"mActionAddWfsLayer"_s );
    addNamedAction( layers, u"mActionLayerProperties"_s );
    addNamedAction( layers, u"mActionRemoveLayer"_s );
    addNamedAction( layers, u"mActionOpenTable"_s );
    addMenu( layers, mApp->layerMenu() );

    QgsAppRibbonGroup *layout = addGroup( page, tr( "Layout" ) );
    addNamedAction( layout, u"mActionNewPrintLayout"_s, true );
    addNamedAction( layout, u"mActionShowLayoutManager"_s );

    QgsAppRibbonGroup *services = addGroup( page, tr( "Services" ) );
    addMenu( services, mApp->databaseMenu() );
    addMenu( services, mApp->webMenu() );

    // Plugins (e.g. DB Manager, MetaSearch) populate these toolbars at runtime.
    mirrorToolbar( addGroup( page, tr( "Database" ) ), mApp->databaseToolBar() );
    mirrorToolbar( addGroup( page, tr( "Web" ) ), mApp->webToolBar() );
  }

  // Analysis
  {
    QgsAppRibbonPage *page = addPage( tr( "Analysis" ) );

    QgsAppRibbonGroup *navigation = addGroup( page, tr( "Navigation" ) );
    addNamedAction( navigation, u"mActionPan"_s, true );

    QgsAppRibbonGroup *measure = addGroup( page, tr( "Measure" ) );
    addNamedAction( measure, u"mActionMeasure"_s, true );
    addNamedAction( measure, u"mActionMeasureArea"_s );
    addNamedAction( measure, u"mActionMeasureBearing"_s );
    addNamedAction( measure, u"mActionMeasureAngle"_s );

    QgsAppRibbonGroup *statistics = addGroup( page, tr( "Statistics" ) );
    addNamedAction( statistics, u"mActionStatisticalSummary"_s );
    addNamedAction( statistics, u"mActionOpenFieldCalc"_s );

    QgsAppRibbonGroup *processing = addGroup( page, tr( "Processing" ) );
    addDockToggle( processing, u"ProcessingToolbox"_s, tr( "Processing Toolbox" ), true );
    addNamedAction( processing, u"mActionShowPythonDialog"_s, true );
    addDeferredMenu( processing, u"processing"_s );
  }

  // Map: navigation, map views, bookmarks and panels
  {
    QgsAppRibbonPage *page = addPage( tr( "Map" ) );
    QgsAppRibbonGroup *navigation = addGroup( page, tr( "Navigation" ) );
    addNamedAction( navigation, u"mActionPan"_s, true );
    addNamedAction( navigation, u"mActionZoomIn"_s );
    addNamedAction( navigation, u"mActionZoomOut"_s );
    addNamedAction( navigation, u"mActionZoomFullExtent"_s );
    addNamedAction( navigation, u"mActionDraw"_s );
    addNamedAction( navigation, u"mActionPanToSelected"_s );
    addNamedAction( navigation, u"mActionZoomToSelected"_s );
    addNamedAction( navigation, u"mActionZoomToLayers"_s );
    addNamedAction( navigation, u"mActionZoomActualSize"_s );
    addNamedAction( navigation, u"mActionZoomLast"_s );
    addNamedAction( navigation, u"mActionZoomNext"_s );

    QgsAppRibbonGroup *mapViews = addGroup( page, tr( "Map Views" ) );
    addNamedAction( mapViews, u"mActionNewMapCanvas"_s, true );

    QgsAppRibbonGroup *views3d = addGroup( page, tr( "3D" ) );
    addNamedAction( views3d, u"mActionNew3DMapCanvas"_s, true );
    addNamedAction( views3d, u"mActionNew3DMapCanvasGlobe"_s, true );

    QgsAppRibbonGroup *bookmarks = addGroup( page, tr( "Bookmarks" ) );
    addNamedAction( bookmarks, u"mActionNewBookmark"_s, true );
    addNamedAction( bookmarks, u"mActionShowBookmarks"_s );

    QgsAppRibbonGroup *panels = addGroup( page, tr( "Panels" ) );
    addDockToggle( panels, u"Browser"_s, tr( "Browser" ) );
    addDockToggle( panels, u"Layers"_s, tr( "Layers" ) );
    addNamedAction( panels, u"mActionTemporalController"_s );
    addNamedAction( panels, u"mActionToggleFullScreen"_s );
    addMenu( panels, mApp->viewMenu() );
  }

  // Vector
  {
    QgsAppRibbonPage *page = addPage( tr( "Vector" ) );

    QgsAppRibbonGroup *navigation = addGroup( page, tr( "Navigation" ) );
    addNamedAction( navigation, u"mActionPan"_s, true );

    QgsAppRibbonGroup *digitize = addGroup( page, tr( "Digitizing" ) );
    addNamedAction( digitize, u"mActionToggleEditing"_s, true );
    addNamedAction( digitize, u"mActionSaveLayerEdits"_s );
    addNamedAction( digitize, u"mActionAddFeature"_s );
    addNamedAction( digitize, u"mActionVertexTool"_s );
    addNamedAction( digitize, u"mActionMoveFeature"_s );
    addNamedAction( digitize, u"mActionDeleteSelected"_s );
    addNamedAction( digitize, u"mActionCutFeatures"_s );
    addNamedAction( digitize, u"mActionCopyFeatures"_s );
    addNamedAction( digitize, u"mActionPasteFeatures"_s );

    QgsAppRibbonGroup *selection = addGroup( page, tr( "Selection" ) );
    addNamedAction( selection, u"mActionSelectFeatures"_s, true );
    addNamedAction( selection, u"mActionSelectPolygon"_s );
    addNamedAction( selection, u"mActionSelectByExpression"_s );
    addNamedAction( selection, u"mActionDeselectAll"_s );

    QgsAppRibbonGroup *labels = addGroup( page, tr( "Labels" ) );
    addNamedAction( labels, u"mActionLabeling"_s, true );
    addNamedAction( labels, u"mActionMoveLabel"_s );
    addNamedAction( labels, u"mActionRotateLabel"_s );
    addNamedAction( labels, u"mActionShowPinnedLabels"_s );
    addNamedAction( labels, u"mActionShowHideLabels"_s );

    QgsAppRibbonGroup *tools = addGroup( page, tr( "Tools" ) );
    addMenu( tools, mApp->vectorMenu() );
  }

  // Raster
  {
    QgsAppRibbonPage *page = addPage( tr( "Raster" ) );
    QgsAppRibbonGroup *stretch = addGroup( page, tr( "Stretch" ) );
    addNamedAction( stretch, u"mActionLocalHistogramStretch"_s, true );
    addNamedAction( stretch, u"mActionFullHistogramStretch"_s );
    addNamedAction( stretch, u"mActionLocalCumulativeCutStretch"_s );
    addNamedAction( stretch, u"mActionFullCumulativeCutStretch"_s );

    QgsAppRibbonGroup *brightness = addGroup( page, tr( "Brightness" ) );
    addNamedAction( brightness, u"mActionIncreaseBrightness"_s );
    addNamedAction( brightness, u"mActionDecreaseBrightness"_s );

    QgsAppRibbonGroup *contrast = addGroup( page, tr( "Contrast" ) );
    addNamedAction( contrast, u"mActionIncreaseContrast"_s );
    addNamedAction( contrast, u"mActionDecreaseContrast"_s );

    QgsAppRibbonGroup *gamma = addGroup( page, tr( "Gamma" ) );
    addNamedAction( gamma, u"mActionIncreaseGamma"_s );
    addNamedAction( gamma, u"mActionDecreaseGamma"_s );

    QgsAppRibbonGroup *tools = addGroup( page, tr( "Tools" ) );
    addNamedAction( tools, u"mActionShowRasterCalculator"_s );
    addMenu( tools, mApp->rasterMenu() );

    QgsAppRibbonGroup *navigation = addGroup( page, tr( "Navigation" ) );
    addNamedAction( navigation, u"mActionPan"_s, true );
  }

  // Extensions: extension management (existing plugin manager), plugin toolbar actions,
  // and top-level menus that installed extensions (e.g. HCMGIS) add at runtime.
  {
    QgsAppRibbonPage *page = addPage( tr( "Extensions" ) );
    QgsAppRibbonGroup *manage = addGroup( page, tr( "Manage" ) );
    addNamedAction( manage, u"mActionManagePlugins"_s, true );
    addNamedAction( manage, u"mActionShowPythonDialog"_s );
    addMenu( manage, mApp->pluginMenu() );

    mirrorToolbar( addGroup( page, tr( "Installed" ) ), mApp->pluginToolBar() );
    mExtensionMenus = addGroup( page, tr( "Installed Menus" ) );
  }

  // Mesh
  {
    QgsAppRibbonPage *page = addPage( tr( "Mesh" ) );
    QgsAppRibbonGroup *layers = addGroup( page, tr( "Layers" ) );
    addNamedAction( layers, u"mActionAddMeshLayer"_s, true );
    addNamedAction( layers, u"mActionNewMeshLayer"_s );

    mirrorToolbar( addGroup( page, tr( "Digitizing" ) ), mApp->meshToolBar() );

    QgsAppRibbonGroup *tools = addGroup( page, tr( "Tools" ) );
    addNamedAction( tools, u"mActionShowMeshCalculator"_s, true );
    addMenu( tools, mApp->meshMenu() );
  }

  // Help
  {
    QgsAppRibbonPage *page = addPage( tr( "Help" ) );
    QgsAppRibbonGroup *help = addGroup( page, tr( "Help" ) );
    addNamedAction( help, u"mActionHelpContents"_s, true );
    addMenu( help, mApp->helpMenu() );
    addNamedAction( help, u"mActionToolSearch"_s );

    QgsAppRibbonGroup *documentation = addGroup( page, tr( "Documentation" ) );
    addNamedAction( documentation, u"mActionHelpAPI"_s );

    QgsAppRibbonGroup *about = addGroup( page, tr( "About" ) );
    addNamedAction( about, u"mActionAbout"_s, true );
    addNamedAction( about, u"mActionQgisHomePage"_s );
  }

  watchMenuBar( mApp->menuBar() );
  refreshOptionalActions();
  updateMetrics();
}

QSize QgsAppRibbon::sizeHint() const
{
  return QSize( tabBar()->sizeHint().width(), tabBar()->sizeHint().height() + mCommandHeight );
}

QSize QgsAppRibbon::minimumSizeHint() const
{
  return QSize( QTabWidget::minimumSizeHint().width(), tabBar()->sizeHint().height() + mCommandHeight );
}

void QgsAppRibbon::updateMetrics()
{
  if ( mUpdatingMetrics || mPages.isEmpty() )
    return;
  mUpdatingMetrics = true;

  ensurePolished();
  const QStyle *st = style();
  QgsAppRibbonMetrics m;
  const int toolBarIcon = st->pixelMetric( QStyle::PM_ToolBarIconSize, nullptr, this );
  m.cellIcon = std::clamp( toolBarIcon, 20, 24 );
  const QFontMetrics fm = fontMetrics();
  m.spaceXs = std::max( 2, fm.height() / 8 );
  m.spaceSm = 2 * m.spaceXs;
  m.spaceMd = 3 * m.spaceXs;
  m.spaceLg = 4 * m.spaceXs;
  m.labelMax = fm.averageCharWidth() * 22;
  m.caretWidth = fm.horizontalAdvance( u"\u25BE"_s ) + m.spaceSm;

  QFont tabFont = font();
  tabFont.setWeight( QFont::Medium );
  tabBar()->setFont( tabFont );

  // Measure real, stylesheet-polished ribbon buttons (a page child picks up the
  // command-area QSS padding and border) instead of guessing sizes.
  {
    QPixmap pixmap( toolBarIcon, toolBarIcon );
    pixmap.fill( Qt::transparent );
    // A labelled large button defines the command-area height (the approved ribbon density),
    // even though commands are presented as icon-only cells.
    QToolButton heightProbe( mPages.first() );
    heightProbe.setObjectName( u"HakeAppRibbonButton"_s );
    heightProbe.setAutoRaise( true );
    heightProbe.setIcon( QIcon( pixmap ) );
    heightProbe.setText( u"Wg"_s );
    heightProbe.setToolButtonStyle( Qt::ToolButtonTextUnderIcon );
    heightProbe.setIconSize( QSize( toolBarIcon, toolBarIcon ) );
    m.tallHeight = heightProbe.sizeHint().height();

    // The cell is the icon plus the theme's button chrome and an even inset, squared.
    QToolButton cellProbe( mPages.first() );
    cellProbe.setObjectName( u"HakeAppRibbonButton"_s );
    cellProbe.setAutoRaise( true );
    cellProbe.setIcon( QIcon( pixmap ) );
    cellProbe.setToolButtonStyle( Qt::ToolButtonIconOnly );
    cellProbe.setIconSize( QSize( m.cellIcon, m.cellIcon ) );
    const QSize chromed = cellProbe.sizeHint();
    m.cellSize = std::min( std::max( chromed.width(), chromed.height() ) + 2 * m.spaceXs, m.tallHeight );
  }
  m.captionHeight = QFontMetrics( ribbonCaptionFont( font() ) ).height();

  if ( mBrand )
  {
    QFont brandFont = ribbonCaptionFont( font() );
    brandFont.setWeight( QFont::DemiBold );
    brandFont.setLetterSpacing( QFont::AbsoluteSpacing, 1.2 );
    mBrand->setFont( brandFont );
    mBrand->parentWidget()->layout()->setContentsMargins( fm.averageCharWidth() * 2, 0, fm.averageCharWidth() * 2, 0 );
    mBrand->parentWidget()->layout()->setSpacing( fm.averageCharWidth() );

    // Sized from the brand caption so these buttons never make the tab strip taller.
    const int iconSize = QFontMetrics( brandFont ).height();
    for ( QToolButton *button : { mThemeToggle, mSearchButton } )
    {
      if ( !button )
        continue;
      button->setIconSize( QSize( iconSize, iconSize ) );
      button->setFixedSize( iconSize + 4, iconSize + 4 );
    }
  }

  if ( mSearchWidget && mSearchField )
  {
    // Room for the placeholder plus the leading search and trailing clear buttons.
    const QFontMetrics searchFm = mSearchField->fontMetrics();
    const int inlineButton = searchFm.height() + 2 * m.spaceSm;
    const int natural = searchFm.horizontalAdvance( mSearchField->placeholderText() ) + 2 * inlineButton + 2 * m.spaceLg;
    mSearchMax = std::clamp( natural, 260, 320 );
    mSearchMin = mSearchMax * 3 / 5;
    mSearchWidget->setMinimumWidth( mSearchMin );
  }

  // Page margins (top SM, bottom XS plus the 1px border), command area, the XS gap above
  // the caption, and the caption row.
  const int chrome = m.spaceSm + m.spaceXs + 1;
  const int command = chrome + m.tallHeight + m.spaceXs + m.captionHeight;
  m.buttonAreaHeight = m.tallHeight;
  m.captions = true;

  if ( m != mMetrics || command != mCommandHeight )
  {
    mMetrics = m;
    mCommandHeight = command;
    for ( QgsAppRibbonPage *page : std::as_const( mPages ) )
      page->setMetrics( m );
  }
  updateGeometry();
  syncChromeTabBarGeometry();
  mUpdatingMetrics = false;
}

bool QgsAppRibbon::event( QEvent *event )
{
  const bool result = QTabWidget::event( event );
  // QTabWidget re-applies the style's corner rect on every layout pass.
  if ( event->type() == QEvent::LayoutRequest )
    syncChromeTabBarGeometry();
#if QT_VERSION >= QT_VERSION_CHECK( 6, 6, 0 )
  if ( event->type() == QEvent::DevicePixelRatioChange )
    updateMetrics();
#endif
  return result;
}

void QgsAppRibbon::changeEvent( QEvent *event )
{
  QTabWidget::changeEvent( event );
  if ( event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange )
    updateMetrics();
}

bool QgsAppRibbon::eventFilter( QObject *watched, QEvent *event )
{
  if ( watched == mSearchField && event->type() == QEvent::KeyPress && mSearchOverlay && mSearchOverlay->isVisible() )
  {
    // Runs before the locator's own filter, which still hides its results on Esc.
    if ( static_cast<QKeyEvent *>( event )->key() == Qt::Key_Escape )
    {
      if ( mSearchEscArmed || mSearchField->text().isEmpty() )
        hideSearchOverlay( true );
      else
        mSearchEscArmed = true;
    }
    else
    {
      mSearchEscArmed = false;
    }
    return QTabWidget::eventFilter( watched, event );
  }

  if ( ( event->type() == QEvent::ActionAdded || event->type() == QEvent::ActionRemoved ) && mMenuBar && watched == mMenuBar.data() )
  {
    // Extensions add or remove top-level menus in bursts; coalesce into one sync.
    if ( !mMenuBarSyncPending )
    {
      mMenuBarSyncPending = true;
      QTimer::singleShot( 0, this, [this] {
        mMenuBarSyncPending = false;
        syncMenuBar();
      } );
    }
  }
  else if ( event->type() == QEvent::ActionAdded || event->type() == QEvent::ActionRemoved )
  {
    auto *toolbar = qobject_cast<QToolBar *>( watched );
    if ( toolbar && mMirroredToolbars.contains( toolbar ) )
    {
      // Plugins often add several actions in a row; coalesce into one rebuild.
      if ( mPendingMirrorSyncs.isEmpty() )
      {
        QTimer::singleShot( 0, this, [this] {
          const QList<QPointer<QToolBar>> pending = std::exchange( mPendingMirrorSyncs, {} );
          for ( const QPointer<QToolBar> &pendingToolbar : pending )
          {
            if ( pendingToolbar )
              syncMirroredGroup( pendingToolbar );
          }
        } );
      }
      if ( !mPendingMirrorSyncs.contains( toolbar ) )
        mPendingMirrorSyncs << toolbar;
    }
  }
  return QTabWidget::eventFilter( watched, event );
}

void QgsAppRibbon::resizeEvent( QResizeEvent *event )
{
  QTabWidget::resizeEvent( event );
  syncChromeTabBarGeometry();
}

void QgsAppRibbon::showEvent( QShowEvent *event )
{
  QTabWidget::showEvent( event );
  syncChromeTabBarGeometry();
}

void QgsAppRibbon::syncChromeTabBarGeometry()
{
  QTabBar *bar = tabBar();
  if ( !bar )
    return;

  // Portable QWidget geometry only (valid on Wayland/X11/Windows/macOS). Document-mode
  // tab bars often keep sizeHint width (= tabs only); force the bar to span up to the
  // brand corner widget so the strip is filled past the last tab without stretching
  // tab labels, and the scroll arrows never sit underneath the brand.
  const int stripWidth = width();
  if ( stripWidth <= 0 )
    return;

  QWidget *filler = cornerWidget( Qt::TopRightCorner );
  syncSearchGeometry( stripWidth, std::max( bar->height(), bar->sizeHint().height() ) );
  const int barWidth = std::max( 1, stripWidth - ( filler ? filler->sizeHint().width() : 0 ) );

  if ( bar->minimumWidth() != barWidth )
    bar->setMinimumWidth( barWidth );

  // Integer height from sizeHint — avoid fighting layout when already the right width.
  const int h = std::max( bar->sizeHint().height(), 1 );
  if ( bar->x() != 0 || bar->width() != barWidth )
  {
    const QRect target( 0, bar->y(), barWidth, std::max( bar->height(), h ) );
    if ( bar->geometry() != target )
      bar->setGeometry( target );
  }

  if ( filler )
  {
    // The style's corner rect is shorter than the tab bar, so a fixed-height filler placed
    // there grows down into the command area; pin it to the tab bar's own row instead.
    const QRect barRect = bar->geometry();
    const int fillerH = barRect.height() > 0 ? barRect.height() : h;
    if ( filler->minimumHeight() != fillerH )
      filler->setMinimumHeight( fillerH );
    if ( filler->maximumHeight() != fillerH )
      filler->setMaximumHeight( fillerH );
    const QRect target( barRect.right() + 1, barRect.top(), std::max( 0, stripWidth - barRect.width() ), fillerH );
    if ( filler->geometry() != target )
      filler->setGeometry( target );
  }
}

void QgsAppRibbon::refreshOptionalActions()
{
  if ( !mApp )
    return;

  for ( QgsAppRibbonPage *page : std::as_const( mPages ) )
  {
    for ( QgsAppRibbonGroup *group : page->groups() )
    {
      if ( group->resolveDeferred( mApp ) )
        group->scheduleRebuild();
    }
  }
}

void QgsAppRibbon::refreshIcons()
{
  for ( QgsAppRibbonPage *page : std::as_const( mPages ) )
  {
    for ( QgsAppRibbonGroup *group : page->groups() )
      group->scheduleRebuild();
  }
}

void QgsAppRibbon::setThemeToggleAction( QAction *action )
{
  if ( !mBrand || !action )
    return;

  if ( !mThemeToggle )
  {
    QWidget *tabFiller = mBrand->parentWidget();
    mThemeToggle = new QToolButton( tabFiller );
    mThemeToggle->setObjectName( u"HakeAppRibbonThemeToggle"_s );
    mThemeToggle->setAutoRaise( true );
    mThemeToggle->setToolButtonStyle( Qt::ToolButtonIconOnly );
    mThemeToggle->setFocusPolicy( Qt::TabFocus );
    if ( QHBoxLayout *fillerLayout = qobject_cast<QHBoxLayout *>( tabFiller->layout() ) )
      fillerLayout->insertWidget( fillerLayout->indexOf( mBrand ), mThemeToggle, 0, Qt::AlignVCenter );
  }
  // Icon and button size are set from the brand font in updateMetrics().
  mThemeToggle->setDefaultAction( action );
}

void QgsAppRibbon::setSearchWidget( QgsLocatorWidget *locator )
{
  if ( !mBrand || !locator || mSearchWidget )
    return;

  QWidget *tabFiller = mBrand->parentWidget();
  auto *fillerLayout = qobject_cast<QHBoxLayout *>( tabFiller->layout() );
  if ( !fillerLayout )
    return;

  const QString accessibleName = tr( "Tool Search" );
  const QString toolTip = tr( "Search tools, layers, and commands" );

  mSearchWidget = locator;
  mSearchField = locator->findChild<QLineEdit *>();
  locator->setPlaceholderText( tr( "Search tools, layers, commands..." ) );
  // Results open beneath the field, right edges aligned, so they stay clear of the window edge.
  locator->setResultContainerAnchors( QgsFloatingWidget::TopRight, QgsFloatingWidget::BottomRight );
  if ( mSearchField )
  {
    mSearchField->setToolTip( toolTip );
    mSearchField->setAccessibleName( accessibleName );
    mSearchField->installEventFilter( this );
  }

  mSearchHost = new QWidget( tabFiller );
  mSearchHost->setObjectName( u"HakeAppRibbonSearch"_s );
  mSearchHost->setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Preferred );
  auto *hostLayout = new QHBoxLayout( mSearchHost );
  hostLayout->setContentsMargins( 0, 0, 0, 0 );
  hostLayout->setSpacing( 0 );

  mSearchButton = new QToolButton( mSearchHost );
  mSearchButton->setObjectName( u"HakeAppRibbonSearchButton"_s );
  mSearchButton->setAutoRaise( true );
  mSearchButton->setToolButtonStyle( Qt::ToolButtonIconOnly );
  mSearchButton->setFocusPolicy( Qt::TabFocus );
  mSearchButton->setToolTip( toolTip );
  mSearchButton->setAccessibleName( accessibleName );
  mSearchButton->setIcon( QgsApplication::getThemeIcon( u"/search.svg"_s ) );
  mSearchButton->hide();
  connect( mSearchButton, &QToolButton::clicked, this, [this] {
    revealSearch();
    if ( mSearchWidget )
      mSearchWidget->search( QString() );
  } );

  hostLayout->addWidget( mSearchButton, 0, Qt::AlignVCenter );
  hostLayout->addWidget( locator, 1, Qt::AlignVCenter );
  fillerLayout->insertWidget( fillerLayout->indexOf( mThemeToggle ? static_cast<QWidget *>( mThemeToggle ) : mBrand ), mSearchHost, 0, Qt::AlignVCenter );

  // Narrow strips: the same locator is shown below the search button, over the workspace.
  mSearchOverlay = new QgsFloatingWidget( window() );
  mSearchOverlay->setObjectName( u"HakeAppRibbonSearchOverlay"_s );
  mSearchOverlay->setAttribute( Qt::WA_StyledBackground, true );
  mSearchOverlay->setAutoFillBackground( true );
  mSearchOverlay->setAnchorWidget( mSearchButton );
  mSearchOverlay->setAnchorPoint( QgsFloatingWidget::TopRight );
  mSearchOverlay->setAnchorWidgetPoint( QgsFloatingWidget::BottomRight );
  new QHBoxLayout( mSearchOverlay );
  mSearchOverlay->hide();

  connect( qApp, &QApplication::focusChanged, this, [this] {
    if ( !mSearchOverlay || !mSearchOverlay->isVisible() )
      return;
    // Deferred so focus moving between the field and the locator results settles first.
    QTimer::singleShot( 0, this, [this] {
      if ( !mSearchOverlay || !mSearchOverlay->isVisible() )
        return;
      QWidget *focus = QApplication::focusWidget();
      // Menus, dialogs and other windows opened from the search keep it open.
      if ( focus && focus->window() != window() )
        return;
      for ( QWidget *w = focus; w; w = w->parentWidget() )
      {
        if ( w == mSearchOverlay )
          return;
        if ( auto *floating = qobject_cast<QgsFloatingWidget *>( w ) )
        {
          if ( mSearchField && floating->anchorWidget() == mSearchField )
            return;
        }
      }
      hideSearchOverlay( false );
    } );
  } );

  updateMetrics();
  syncChromeTabBarGeometry();
}

void QgsAppRibbon::revealSearch()
{
  if ( !mSearchOverlay || !mSearchCollapsed || !mSearchVisible )
    return;

  if ( !mSearchOverlay->isVisible() )
  {
    QWidget *focus = QApplication::focusWidget();
    mFocusBeforeSearch = focus && !mSearchOverlay->isAncestorOf( focus ) ? focus : nullptr;
  }
  mSearchEscArmed = false;

  const int margin = mMetrics.spaceSm;
  if ( QLayout *overlayLayout = mSearchOverlay->layout() )
    overlayLayout->setContentsMargins( margin, margin, margin, margin );
  const int available = window()->width() - 2 * margin;
  mSearchOverlay->setFixedWidth( std::max( mSearchMin, std::min( mSearchMax, available ) ) );
  mSearchOverlay->adjustSize();
  mSearchOverlay->show();
  mSearchOverlay->raise();
}

void QgsAppRibbon::setSearchVisible( bool visible )
{
  mSearchVisible = visible;
  if ( !mSearchHost )
    return;

  if ( !visible )
    hideSearchOverlay( false );
  mSearchHost->setVisible( visible );
  syncChromeTabBarGeometry();
}

void QgsAppRibbon::syncSearchGeometry( int stripWidth, int rowHeight )
{
  // Search sizes come from updateMetrics(), which needs the ribbon pages.
  if ( !mSearchHost || !mSearchWidget || !mSearchVisible || !mBrand || mSearchMax <= 0 )
    return;

  const auto layoutWidth = []( const QWidget *w ) { return std::clamp( w->sizeHint().width(), w->minimumWidth(), w->maximumWidth() ); };

  QLayout *fillerLayout = mBrand->parentWidget()->layout();
  const QMargins margins = fillerLayout->contentsMargins();
  const int spacing = std::max( 0, fillerLayout->spacing() );
  int corner = margins.left() + margins.right() + layoutWidth( mBrand ) + spacing;
  if ( mThemeToggle && !mThemeToggle->isHidden() )
    corner += layoutWidth( mThemeToggle ) + spacing;

  // The tabs keep their natural width; the search takes what is left, or yields to a button.
  const int spare = stripWidth - tabBar()->sizeHint().width() - corner;
  const bool collapse = spare < mSearchMin;
  setSearchCollapsed( collapse );

  const int hostWidth = collapse ? layoutWidth( mSearchButton ) : std::min( spare, mSearchMax );
  if ( mSearchHost->minimumWidth() != hostWidth || mSearchHost->maximumWidth() != hostWidth )
    mSearchHost->setFixedWidth( hostWidth );

  const int fieldHeight = std::max( 1, rowHeight - 2 * mMetrics.spaceXs );
  if ( mSearchWidget->maximumHeight() != fieldHeight )
    mSearchWidget->setMaximumHeight( fieldHeight );
}

void QgsAppRibbon::setSearchCollapsed( bool collapsed )
{
  if ( !mSearchWidget || collapsed == mSearchCollapsed )
    return;

  mSearchCollapsed = collapsed;
  hideSearchOverlay( false );
  if ( collapsed )
  {
    mSearchOverlay->layout()->addWidget( mSearchWidget );
    mSearchButton->show();
  }
  else
  {
    static_cast<QHBoxLayout *>( mSearchHost->layout() )->addWidget( mSearchWidget, 1, Qt::AlignVCenter );
    mSearchButton->hide();
  }
  mSearchWidget->show();
}

void QgsAppRibbon::hideSearchOverlay( bool restoreFocus )
{
  if ( !mSearchOverlay || !mSearchOverlay->isVisible() )
    return;

  mSearchOverlay->hide();
  if ( restoreFocus && mFocusBeforeSearch )
    mFocusBeforeSearch->setFocus( Qt::OtherFocusReason );
  mFocusBeforeSearch = nullptr;
}

QgsAppRibbonPage *QgsAppRibbon::addPage( const QString &title )
{
  auto *page = new QgsAppRibbonPage( this );
  addTab( page, title );
  mPages << page;
  return page;
}

QgsAppRibbonGroup *QgsAppRibbon::addGroup( QgsAppRibbonPage *page, const QString &title )
{
  return page->addGroup( title );
}

void QgsAppRibbon::addNamedAction( QgsAppRibbonGroup *group, const QString &objectName, bool primary )
{
  if ( !mApp || !group )
    return;
  if ( QAction *action = mApp->findChild<QAction *>( objectName ) )
    group->addEntry( { action, primary, QString(), QString(), QString() } );
}

void QgsAppRibbon::addDockToggle( QgsAppRibbonGroup *group, const QString &dockObjectName, const QString &label, bool primary )
{
  if ( group )
    group->addEntry( { nullptr, primary, dockObjectName, QString(), label } );
}

void QgsAppRibbon::addMenu( QgsAppRibbonGroup *group, QMenu *menu )
{
  if ( group && menu )
    group->addEntry( { menu->menuAction(), false, QString(), QString(), QString() } );
}

void QgsAppRibbon::addDeferredMenu( QgsAppRibbonGroup *group, const QString &menuObjectName )
{
  if ( group )
    group->addEntry( { nullptr, false, QString(), menuObjectName, QString() } );
}

void QgsAppRibbon::mirrorToolbar( QgsAppRibbonGroup *group, QToolBar *toolbar )
{
  if ( !group || !toolbar )
    return;
  mMirroredToolbars.insert( toolbar, group );
  toolbar->installEventFilter( this );
  const QList<QAction *> actions = toolbar->actions();
  for ( QAction *action : actions )
  {
    if ( QAction *presented = presentableToolbarAction( action ) )
      group->addEntry( { presented, false, QString(), QString(), QString() } );
  }
}

void QgsAppRibbon::syncMirroredGroup( QToolBar *toolbar )
{
  QgsAppRibbonGroup *group = mMirroredToolbars.value( toolbar );
  if ( !group )
    return;
  QList<QgsAppRibbonGroup::Entry> entries;
  const QList<QAction *> actions = toolbar->actions();
  for ( QAction *action : actions )
  {
    if ( QAction *presented = presentableToolbarAction( action ) )
      entries.append( QgsAppRibbonGroup::Entry { presented, false, QString(), QString(), QString() } );
  }
  group->setEntries( entries );
}

void QgsAppRibbon::watchMenuBar( QMenuBar *menuBar )
{
  if ( !menuBar )
    return;
  mMenuBar = menuBar;
  menuBar->installEventFilter( this );
  syncMenuBar();
}

bool QgsAppRibbon::isStandardMenu( const QMenu *menu ) const
{
  if ( !mApp || !menu )
    return false;
  // Processing is a core plugin with its own drop-down in the Analysis tab.
  if ( menu->objectName() == "processing"_L1 )
    return true;
  const QList<const QMenu *> standardMenus {
    mApp->projectMenu(),
    mApp->editMenu(),
    mApp->viewMenu(),
    mApp->layerMenu(),
    mApp->settingsMenu(),
    mApp->pluginMenu(),
    mApp->vectorMenu(),
    mApp->rasterMenu(),
    mApp->databaseMenu(),
    mApp->webMenu(),
    mApp->meshMenu(),
    mApp->windowMenu(),
    mApp->helpMenu(),
  };
  return standardMenus.contains( menu );
}

void QgsAppRibbon::syncMenuBar()
{
  if ( !mApp || !mMenuBar )
    return;

  const QList<QAction *> barActions = mMenuBar->actions();
  const QList<QAction *> appActions = mApp->actions();
  for ( const QPointer<QAction> &adopted : std::as_const( mAdoptedMenuBarActions ) )
  {
    if ( adopted && !barActions.contains( adopted.data() ) )
      mApp->removeAction( adopted );
  }
  mAdoptedMenuBarActions.clear();

  QList<QgsAppRibbonGroup::Entry> extensionEntries;
  for ( QAction *action : barActions )
  {
    // The menu bar is hidden; registering its menus on the main window keeps their shortcuts active.
    if ( !appActions.contains( action ) )
      mApp->addAction( action );
    mAdoptedMenuBarActions << action;

    QMenu *menu = action->menu();
    if ( menu && !isStandardMenu( menu ) )
      extensionEntries.append( QgsAppRibbonGroup::Entry { action, false, QString(), QString(), QString() } );
  }

  if ( mExtensionMenus )
    mExtensionMenus->setEntries( extensionEntries );
}
