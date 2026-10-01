import { ComponentFixture, TestBed } from '@angular/core/testing';
import { HttpClientTestingModule } from '@angular/common/http/testing';
import { RouterTestingModule } from '@angular/router/testing';

import { configureTestBed } from '~/testing/unit-test-helper';
import { SharedModule } from '../../shared.module';
import { StorageInsightsNotificationComponent } from './storage-insights-notification.component';

describe('StorageInsightsNotificationComponent', () => {
  let component: StorageInsightsNotificationComponent;
  let fixture: ComponentFixture<StorageInsightsNotificationComponent>;

  configureTestBed({
    declarations: [StorageInsightsNotificationComponent],
    imports: [HttpClientTestingModule, RouterTestingModule, SharedModule]
  });

  beforeEach(() => {
    fixture = TestBed.createComponent(StorageInsightsNotificationComponent);
    component = fixture.componentInstance;
    fixture.detectChanges();
  });

  it('should create', () => {
    expect(component).toBeTruthy();
  });
});
